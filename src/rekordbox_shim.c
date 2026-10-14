#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sqlcipher/sqlite3.h>

#define SHIM_VERSION "1.0.0"

#define EXPORT __attribute__((visibility("default"))) JNIEXPORT
#define LOG(fmt, ...) do { printf("[rekordbox-shim] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)

#define DEFAULT_DB_PATH "/home/localadmin/Library/Pioneer/rekordbox/master.db"
#define ANLZ_BASE_DIR   "/home/localadmin/Library/Pioneer/rekordbox/share"
#define REKORDBOX_HEX_KEY "402fd482c38817c35ffa8ffb8c7d93143b749e7d315df7a81732a1ff43608497"

static sqlite3 *db = NULL;

/* Safe unaligned big-endian readers */
static inline uint16_t read_be16(const uint8_t *p) {
    uint16_t val;
    memcpy(&val, p, sizeof(val));
    return ntohs(val);
}

static inline uint32_t read_be32(const uint8_t *p) {
    uint32_t val;
    memcpy(&val, p, sizeof(val));
    return ntohl(val);
}

/* Helper to check if str ends with suffix */
static int ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    size_t len_str = strlen(str);
    size_t len_suf = strlen(suffix);
    if (len_suf > len_str) return 0;
    return strcmp(str + len_str - len_suf, suffix) == 0;
}

static sqlite3* get_db_handle(void) {
    if (db) return db;

    const char *db_path = getenv("REKORDBOX_DB_PATH");
    if (!db_path || strlen(db_path) == 0) {
        db_path = DEFAULT_DB_PATH;
    }

    LOG("Opening database: %s", db_path);
    int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(db_path, &db, flags, NULL) != SQLITE_OK) {
        LOG("Failed to open SQLite database: %s", db ? sqlite3_errmsg(db) : "allocation error");
        if (db) {
            sqlite3_close(db);
            db = NULL;
        }
        return NULL;
    }

    // Authenticate using PRAGMA key
    char pragma_cmd[128];
    snprintf(pragma_cmd, sizeof(pragma_cmd), "PRAGMA key = '%s';", REKORDBOX_HEX_KEY);
    char *err_msg = NULL;
    if (sqlite3_exec(db, pragma_cmd, NULL, NULL, &err_msg) != SQLITE_OK) {
        LOG("PRAGMA key failed: %s", err_msg ? err_msg : "unknown error");
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_close(db);
        db = NULL;
        return NULL;
    }

    // Decryption verification check
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM djmdContent WHERE rb_local_deleted = 0;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            LOG("Successfully unlocked master.db! Available tracks: %d", sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
    } else {
        LOG("Decryption check failed: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
        return NULL;
    }

    return db;
}

static jobject create_dummy_beatgrid(JNIEnv *env, double bpm_val, int length_sec) {
    jclass bgClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSDK$BeatGrids");
    if (!bgClass) return NULL;

    jmethodID bgCtor = (*env)->GetMethodID(env, bgClass, "<init>", "(II[J[F)V");
    if (!bgCtor) {
        (*env)->DeleteLocalRef(env, bgClass);
        return NULL;
    }

    if (bpm_val <= 0.0) bpm_val = 120.0;
    if (length_sec <= 0) length_sec = 300;

    double beat_interval_ms = (60.0 / bpm_val) * 1000.0;
    int total_beats = (int)((length_sec * 1000.0) / beat_interval_ms) + 16;
    if (total_beats < 16) total_beats = 16;

    jlongArray beatposArr = (*env)->NewLongArray(env, total_beats);
    jfloatArray bpmArr = (*env)->NewFloatArray(env, total_beats);
    if (!beatposArr || !bpmArr) {
        if (beatposArr) (*env)->DeleteLocalRef(env, beatposArr);
        if (bpmArr) (*env)->DeleteLocalRef(env, bpmArr);
        (*env)->DeleteLocalRef(env, bgClass);
        return NULL;
    }

    jlong *beatpos_elems = (jlong*)malloc(sizeof(jlong) * total_beats);
    jfloat *bpm_elems = (jfloat*)malloc(sizeof(jfloat) * total_beats);
    if (!beatpos_elems || !bpm_elems) {
        free(beatpos_elems);
        free(bpm_elems);
        (*env)->DeleteLocalRef(env, beatposArr);
        (*env)->DeleteLocalRef(env, bpmArr);
        (*env)->DeleteLocalRef(env, bgClass);
        return NULL;
    }

    for (int i = 0; i < total_beats; i++) {
        beatpos_elems[i] = (jlong)(i * beat_interval_ms);
        bpm_elems[i] = (jfloat)bpm_val;
    }

    (*env)->SetLongArrayRegion(env, beatposArr, 0, total_beats, beatpos_elems);
    (*env)->SetFloatArrayRegion(env, bpmArr, 0, total_beats, bpm_elems);

    free(beatpos_elems);
    free(bpm_elems);

    jobject beatGrid = (*env)->NewObject(env, bgClass, bgCtor, total_beats, 1, beatposArr, bpmArr);

    (*env)->DeleteLocalRef(env, beatposArr);
    (*env)->DeleteLocalRef(env, bpmArr);
    (*env)->DeleteLocalRef(env, bgClass);

    return beatGrid;
}

static jobject parse_anlz_beatgrid(JNIEnv *env, const char *anlz_rel_path, double fallback_bpm) {
    if (!anlz_rel_path || strlen(anlz_rel_path) == 0) return NULL;

    char full_anlz_path[1024];
    snprintf(full_anlz_path, sizeof(full_anlz_path), "%s%s", ANLZ_BASE_DIR, anlz_rel_path);

    FILE *f = fopen(full_anlz_path, "rb");
    if (!f) {
        LOG("Could not open ANLZ file: %s", full_anlz_path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long fsize = ftell(f);
    if (fsize < 64 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *buf = (uint8_t*)malloc(fsize);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    const uint8_t *buf_end = buf + fsize;

    // Locate PQTZ section safely within buffer bounds
    const uint8_t *pqtz = NULL;
    for (long i = 0; i <= fsize - 24; i++) {
        if (memcmp(buf + i, "PQTZ", 4) == 0) {
            pqtz = buf + i;
            break;
        }
    }

    if (!pqtz) {
        LOG("PQTZ section not found in %s", full_anlz_path);
        free(buf);
        return NULL;
    }

    uint32_t header_len = read_be32(pqtz + 4);
    uint32_t section_len = read_be32(pqtz + 8);
    uint32_t beat_count = read_be32(pqtz + 20);

    // Validate header and section bounds against physical buffer end
    if (header_len < 24 || section_len < header_len || pqtz + header_len > buf_end) {
        LOG("Malformed PQTZ header in %s", full_anlz_path);
        free(buf);
        return NULL;
    }

    size_t max_available_bytes = (size_t)(buf_end - (pqtz + header_len));
    size_t section_payload_bytes = (size_t)(section_len - header_len);
    if (section_payload_bytes > max_available_bytes) {
        section_payload_bytes = max_available_bytes;
    }

    uint32_t max_possible_beats = (uint32_t)(section_payload_bytes / 8);
    if (beat_count == 0 || beat_count > max_possible_beats) {
        beat_count = max_possible_beats;
    }

    if (beat_count == 0 || beat_count > 50000) {
        LOG("Invalid beat count %u in %s", beat_count, full_anlz_path);
        free(buf);
        return NULL;
    }

    const uint8_t *entries = pqtz + header_len;

    jclass bgClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSDK$BeatGrids");
    if (!bgClass) {
        free(buf);
        return NULL;
    }

    jmethodID bgCtor = (*env)->GetMethodID(env, bgClass, "<init>", "(II[J[F)V");
    if (!bgCtor) {
        (*env)->DeleteLocalRef(env, bgClass);
        free(buf);
        return NULL;
    }

    jlongArray beatposArr = (*env)->NewLongArray(env, beat_count);
    jfloatArray bpmArr = (*env)->NewFloatArray(env, beat_count);
    if (!beatposArr || !bpmArr) {
        if (beatposArr) (*env)->DeleteLocalRef(env, beatposArr);
        if (bpmArr) (*env)->DeleteLocalRef(env, bpmArr);
        (*env)->DeleteLocalRef(env, bgClass);
        free(buf);
        return NULL;
    }

    jlong *beatpos_elems = (jlong*)malloc(sizeof(jlong) * beat_count);
    jfloat *bpm_elems = (jfloat*)malloc(sizeof(jfloat) * beat_count);
    if (!beatpos_elems || !bpm_elems) {
        free(beatpos_elems);
        free(bpm_elems);
        (*env)->DeleteLocalRef(env, beatposArr);
        (*env)->DeleteLocalRef(env, bpmArr);
        (*env)->DeleteLocalRef(env, bgClass);
        free(buf);
        return NULL;
    }

    int first_beat_offset = 1;

    for (uint32_t b = 0; b < beat_count; b++) {
        const uint8_t *entry = entries + (b * 8);
        uint16_t beat_num = read_be16(entry + 0);
        uint16_t raw_bpm = read_be16(entry + 2);
        uint32_t raw_time_ms = read_be32(entry + 4);

        if (b == 0) {
            first_beat_offset = (beat_num > 0 && beat_num <= 4) ? beat_num : 1;
        }

        beatpos_elems[b] = (jlong)raw_time_ms;
        bpm_elems[b] = (raw_bpm > 0) ? ((float)raw_bpm / 100.0f) : (float)fallback_bpm;
    }

    (*env)->SetLongArrayRegion(env, beatposArr, 0, beat_count, beatpos_elems);
    (*env)->SetFloatArrayRegion(env, bpmArr, 0, beat_count, bpm_elems);

    free(beatpos_elems);
    free(bpm_elems);
    free(buf);

    jobject beatGrid = (*env)->NewObject(env, bgClass, bgCtor, (jint)beat_count, (jint)first_beat_offset, beatposArr, bpmArr);

    (*env)->DeleteLocalRef(env, beatposArr);
    (*env)->DeleteLocalRef(env, bpmArr);
    (*env)->DeleteLocalRef(env, bgClass);

    LOG("Successfully extracted %u Rekordbox beat points (downbeat offset: %d) from %s", beat_count, first_beat_offset, anlz_rel_path);
    return beatGrid;
}

EXPORT jint JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_initNative(JNIEnv *env, jobject obj) {
    LOG("v%s initialized (initNative called)", SHIM_VERSION);
    get_db_handle();
    return 0;
}

EXPORT void JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_deinitNative(JNIEnv *env, jobject obj) {
    LOG("deinitNative called");
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

EXPORT void JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_dump(JNIEnv *env, jobject obj) {
    LOG("dump called");
}

EXPORT jobjectArray JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongs(JNIEnv *env, jobject obj) {
    LOG("getSongs called");
    sqlite3 *h = get_db_handle();

    jclass songClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSong");
    if (!songClass) return NULL;

    jmethodID ctor = (*env)->GetMethodID(env, songClass, "<init>", 
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V");
    if (!ctor) {
        (*env)->DeleteLocalRef(env, songClass);
        return NULL;
    }

    if (!h) {
        jobjectArray empty = (*env)->NewObjectArray(env, 0, songClass, NULL);
        (*env)->DeleteLocalRef(env, songClass);
        return empty;
    }

    int total_tracks = 0;
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(h, "SELECT count(*) FROM djmdContent WHERE rb_local_deleted = 0;", -1, &count_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            total_tracks = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }

    jobjectArray songArray = (*env)->NewObjectArray(env, total_tracks, songClass, NULL);
    if (!songArray) {
        (*env)->DeleteLocalRef(env, songClass);
        return NULL;
    }

    const char *query = 
        "SELECT c.ID, c.Title, a.Name, al.Name, c.BPM, g.Name, k.ScaleName, c.FolderPath, c.FileNameL "
        "FROM djmdContent c "
        "LEFT JOIN djmdArtist a ON c.ArtistID = a.ID "
        "LEFT JOIN djmdAlbum al ON c.AlbumID = al.ID "
        "LEFT JOIN djmdGenre g ON c.GenreID = g.ID "
        "LEFT JOIN djmdKey k ON c.KeyID = k.ID "
        "WHERE c.rb_local_deleted = 0;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, query, -1, &stmt, NULL) != SQLITE_OK) {
        LOG("getSongs query prepare error: %s", sqlite3_errmsg(h));
        (*env)->DeleteLocalRef(env, songClass);
        return songArray;
    }

    jstring fileType = (*env)->NewStringUTF(env, "AUDIO");

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total_tracks) {
        const char *id_val = (const char*)sqlite3_column_text(stmt, 0);
        const char *title_val = (const char*)sqlite3_column_text(stmt, 1);
        const char *artist_val = (const char*)sqlite3_column_text(stmt, 2);
        const char *album_val = (const char*)sqlite3_column_text(stmt, 3);
        double bpm_val = sqlite3_column_double(stmt, 4) / 100.0;
        const char *genre_val = (const char*)sqlite3_column_text(stmt, 5);
        const char *key_val = (const char*)sqlite3_column_text(stmt, 6);
        const char *folder_val = (const char*)sqlite3_column_text(stmt, 7);
        const char *file_val = (const char*)sqlite3_column_text(stmt, 8);

        char full_path[1024] = {0};
        if (folder_val && strlen(folder_val) > 0) {
            if (file_val && strlen(file_val) > 0 && !ends_with(folder_val, file_val)) {
                size_t flen = strlen(folder_val);
                if (folder_val[flen - 1] == '/') {
                    snprintf(full_path, sizeof(full_path), "%s%s", folder_val, file_val);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", folder_val, file_val);
                }
            } else {
                strncpy(full_path, folder_val, sizeof(full_path) - 1);
            }
        } else if (file_val) {
            strncpy(full_path, file_val, sizeof(full_path) - 1);
        }

        jstring id = (*env)->NewStringUTF(env, id_val ? id_val : "");
        jstring title = (*env)->NewStringUTF(env, title_val ? title_val : "Untitled");
        jstring artist = (*env)->NewStringUTF(env, artist_val ? artist_val : "Unknown Artist");
        jstring album = (*env)->NewStringUTF(env, album_val ? album_val : "");
        jstring genre = (*env)->NewStringUTF(env, genre_val ? genre_val : "");
        jstring key = (*env)->NewStringUTF(env, key_val ? key_val : "");
        jstring path = (*env)->NewStringUTF(env, full_path);
        jstring rawPath = (*env)->NewStringUTF(env, full_path);
        jboolean isAvailable = JNI_TRUE;

        jobject song = (*env)->NewObject(env, songClass, ctor, id, title, artist, album, bpm_val, genre, key, path, rawPath, fileType, isAvailable);
        (*env)->SetObjectArrayElement(env, songArray, idx, song);

        (*env)->DeleteLocalRef(env, id);
        (*env)->DeleteLocalRef(env, title);
        (*env)->DeleteLocalRef(env, artist);
        (*env)->DeleteLocalRef(env, album);
        (*env)->DeleteLocalRef(env, genre);
        (*env)->DeleteLocalRef(env, key);
        (*env)->DeleteLocalRef(env, path);
        (*env)->DeleteLocalRef(env, rawPath);
        (*env)->DeleteLocalRef(env, song);

        idx++;
    }

    (*env)->DeleteLocalRef(env, fileType);
    (*env)->DeleteLocalRef(env, songClass);
    sqlite3_finalize(stmt);
    LOG("Delivered %d tracks to Bitwig", idx);
    return songArray;
}

EXPORT jobjectArray JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getRawPlaylists(JNIEnv *env, jobject obj) {
    LOG("getRawPlaylists called");
    sqlite3 *h = get_db_handle();

    jclass plClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSDK$RawPlaylist");
    if (!plClass) return NULL;

    jmethodID ctor = (*env)->GetMethodID(env, plClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V");
    if (!ctor) {
        (*env)->DeleteLocalRef(env, plClass);
        return NULL;
    }

    if (!h) {
        jobjectArray empty = (*env)->NewObjectArray(env, 0, plClass, NULL);
        (*env)->DeleteLocalRef(env, plClass);
        return empty;
    }

    int total_pl = 0;
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(h, "SELECT count(*) FROM djmdPlaylist WHERE rb_local_deleted = 0;", -1, &count_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            total_pl = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }

    jobjectArray plArray = (*env)->NewObjectArray(env, total_pl, plClass, NULL);
    if (!plArray) {
        (*env)->DeleteLocalRef(env, plClass);
        return NULL;
    }

    const char *query = "SELECT ID, ParentID, Name, Attribute FROM djmdPlaylist WHERE rb_local_deleted = 0 ORDER BY Seq, ID;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, query, -1, &stmt, NULL) != SQLITE_OK) {
        (*env)->DeleteLocalRef(env, plClass);
        return plArray;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < total_pl) {
        const char *id_val = (const char*)sqlite3_column_text(stmt, 0);
        const char *parent_val = (const char*)sqlite3_column_text(stmt, 1);
        const char *name_val = (const char*)sqlite3_column_text(stmt, 2);
        int attr = sqlite3_column_int(stmt, 3);
        int is_folder = (attr == 1);

        jstring plId = (*env)->NewStringUTF(env, id_val ? id_val : "");
        jstring plParent = (parent_val && strcmp(parent_val, "root") != 0) ? (*env)->NewStringUTF(env, parent_val) : NULL;
        jstring plName = (*env)->NewStringUTF(env, name_val ? name_val : "Playlist");
        jboolean folderFlag = (is_folder != 0) ? JNI_TRUE : JNI_FALSE;

        jobject pl = (*env)->NewObject(env, plClass, ctor, plId, plParent, plName, folderFlag);
        (*env)->SetObjectArrayElement(env, plArray, idx, pl);

        (*env)->DeleteLocalRef(env, plId);
        if (plParent) (*env)->DeleteLocalRef(env, plParent);
        (*env)->DeleteLocalRef(env, plName);
        (*env)->DeleteLocalRef(env, pl);

        idx++;
    }

    (*env)->DeleteLocalRef(env, plClass);
    sqlite3_finalize(stmt);
    LOG("Delivered %d playlists to Bitwig", idx);
    return plArray;
}

EXPORT jobjectArray JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongIDsInPlaylist(JNIEnv *env, jobject obj, jstring plId) {
    sqlite3 *h = get_db_handle();
    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    if (!stringClass) return NULL;

    if (!h || !plId) {
        jobjectArray empty = (*env)->NewObjectArray(env, 0, stringClass, NULL);
        (*env)->DeleteLocalRef(env, stringClass);
        return empty;
    }

    const char *playlist_id = (*env)->GetStringUTFChars(env, plId, NULL);
    if (!playlist_id) {
        (*env)->DeleteLocalRef(env, stringClass);
        return NULL;
    }

    int total_songs = 0;
    sqlite3_stmt *count_stmt = NULL;
    const char *count_query = "SELECT count(*) FROM djmdSongPlaylist WHERE PlaylistID = ? AND rb_local_deleted = 0;";
    if (sqlite3_prepare_v2(h, count_query, -1, &count_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(count_stmt, 1, playlist_id, -1, SQLITE_STATIC);
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            total_songs = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }

    jobjectArray idArray = (*env)->NewObjectArray(env, total_songs, stringClass, NULL);
    if (!idArray) {
        (*env)->ReleaseStringUTFChars(env, plId, playlist_id);
        (*env)->DeleteLocalRef(env, stringClass);
        return NULL;
    }

    const char *query = "SELECT ContentID FROM djmdSongPlaylist WHERE PlaylistID = ? AND rb_local_deleted = 0 ORDER BY TrackNo, ID;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, query, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, playlist_id, -1, SQLITE_STATIC);
        int idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && idx < total_songs) {
            const char *cid = (const char*)sqlite3_column_text(stmt, 0);
            jstring songId = (*env)->NewStringUTF(env, cid ? cid : "");
            (*env)->SetObjectArrayElement(env, idArray, idx, songId);
            (*env)->DeleteLocalRef(env, songId);
            idx++;
        }
        sqlite3_finalize(stmt);
    }

    (*env)->ReleaseStringUTFChars(env, plId, playlist_id);
    (*env)->DeleteLocalRef(env, stringClass);
    return idArray;
}

EXPORT jobject JNICALL Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongDetails(JNIEnv *env, jobject obj, jobject song) {
    LOG("getSongDetails called");
    if (!song) return NULL;

    sqlite3 *h = get_db_handle();

    // 1. Get Song ID from RekordboxSong
    jclass songClass = (*env)->GetObjectClass(env, song);
    jmethodID getIDMethod = (*env)->GetMethodID(env, songClass, "getID", "()Ljava/lang/String;");
    if (!getIDMethod) {
        (*env)->DeleteLocalRef(env, songClass);
        return NULL;
    }

    jstring songIDStr = (jstring)(*env)->CallObjectMethod(env, song, getIDMethod);
    (*env)->DeleteLocalRef(env, songClass);
    const char *content_id = songIDStr ? (*env)->GetStringUTFChars(env, songIDStr, NULL) : NULL;

    // 2. Fetch BPM, Length, and AnalysisDataPath from djmdContent
    double track_bpm = 120.0;
    int track_len = 300;
    char anlz_path[512] = {0};

    if (h && content_id) {
        sqlite3_stmt *bstmt = NULL;
        const char *info_sql = "SELECT BPM, Length, AnalysisDataPath FROM djmdContent WHERE ID = ?;";
        if (sqlite3_prepare_v2(h, info_sql, -1, &bstmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(bstmt, 1, content_id, -1, SQLITE_STATIC);
            if (sqlite3_step(bstmt) == SQLITE_ROW) {
                track_bpm = sqlite3_column_double(bstmt, 0) / 100.0;
                track_len = sqlite3_column_int(bstmt, 1);
                const char *a_val = (const char*)sqlite3_column_text(bstmt, 2);
                if (a_val) {
                    strncpy(anlz_path, a_val, sizeof(anlz_path) - 1);
                }
            }
            sqlite3_finalize(bstmt);
        }
    }

    // 3. Extract genuine Rekordbox BeatGrid (fallback to synthetic if unavailable)
    jobject beatGridObj = NULL;
    if (strlen(anlz_path) > 0) {
        beatGridObj = parse_anlz_beatgrid(env, anlz_path, track_bpm);
    }
    if (!beatGridObj) {
        beatGridObj = create_dummy_beatgrid(env, track_bpm, track_len);
    }

    // 4. Query Cue markers from djmdCue
    jclass cueClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSongDetails$CueMarker");
    jmethodID cueCtor = cueClass ? (*env)->GetMethodID(env, cueClass, "<init>", "(IJ)V") : NULL;

    int cue_count = 0;
    jobjectArray cueArray = NULL;

    if (h && content_id && cueClass && cueCtor) {
        sqlite3_stmt *cstmt = NULL;
        const char *count_sql = "SELECT count(*) FROM djmdCue WHERE ContentID = ? AND rb_local_deleted = 0 AND InMsec >= 0;";
        if (sqlite3_prepare_v2(h, count_sql, -1, &cstmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(cstmt, 1, content_id, -1, SQLITE_STATIC);
            if (sqlite3_step(cstmt) == SQLITE_ROW) {
                cue_count = sqlite3_column_int(cstmt, 0);
            }
            sqlite3_finalize(cstmt);
        }

        cueArray = (*env)->NewObjectArray(env, cue_count, cueClass, NULL);

        if (cue_count > 0 && cueArray) {
            const char *cue_sql = "SELECT Kind, InMsec, OutMsec FROM djmdCue WHERE ContentID = ? AND rb_local_deleted = 0 AND InMsec >= 0 ORDER BY InMsec ASC;";
            if (sqlite3_prepare_v2(h, cue_sql, -1, &cstmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(cstmt, 1, content_id, -1, SQLITE_STATIC);
                int idx = 0;
                while (sqlite3_step(cstmt) == SQLITE_ROW && idx < cue_count) {
                    int kind = sqlite3_column_int(cstmt, 0);
                    jlong pos_ms = (jlong)sqlite3_column_int64(cstmt, 1);
                    jlong out_ms = (jlong)sqlite3_column_int64(cstmt, 2);

                    // If Memory Cue (0) has a valid loop end (OutMsec > InMsec), Bitwig's Loop kind is -1
                    if (kind == 0 && out_ms > pos_ms) {
                        kind = -1;
                    }

                    jobject cueObj = (*env)->NewObject(env, cueClass, cueCtor, (jint)kind, pos_ms);
                    (*env)->SetObjectArrayElement(env, cueArray, idx, cueObj);
                    (*env)->DeleteLocalRef(env, cueObj);
                    idx++;
                }
                sqlite3_finalize(cstmt);
            }
        }
    }

    if (songIDStr && content_id) {
        (*env)->ReleaseStringUTFChars(env, songIDStr, content_id);
    }
    if (songIDStr) (*env)->DeleteLocalRef(env, songIDStr);

    if (!cueArray && cueClass) {
        cueArray = (*env)->NewObjectArray(env, 0, cueClass, NULL);
    }
    if (cueClass) (*env)->DeleteLocalRef(env, cueClass);

    // 5. Empty PhraseInfo array
    jclass phraseClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSDK$PhraseInfo");
    jobjectArray phraseArray = phraseClass ? (*env)->NewObjectArray(env, 0, phraseClass, NULL) : NULL;
    if (phraseClass) (*env)->DeleteLocalRef(env, phraseClass);

    // 6. Instantiate RekordboxSongDetails
    jclass detailsClass = (*env)->FindClass(env, "com/bitwig/base/rekordbox/library/RekordboxSongDetails");
    if (!detailsClass) {
        if (beatGridObj) (*env)->DeleteLocalRef(env, beatGridObj);
        if (phraseArray) (*env)->DeleteLocalRef(env, phraseArray);
        if (cueArray) (*env)->DeleteLocalRef(env, cueArray);
        return NULL;
    }

    jmethodID detailsCtor = (*env)->GetMethodID(env, detailsClass, "<init>",
        "(Lcom/bitwig/base/rekordbox/library/RekordboxSong;"
        "Lcom/bitwig/base/rekordbox/library/RekordboxSDK$BeatGrids;"
        "Lcom/bitwig/base/rekordbox/library/RekordboxSDK$VocalDetectInfo;"
        "I"
        "[Lcom/bitwig/base/rekordbox/library/RekordboxSDK$PhraseInfo;"
        "[Lcom/bitwig/base/rekordbox/library/RekordboxSongDetails$CueMarker;)V");

    if (!detailsCtor) {
        LOG("Could not find RekordboxSongDetails constructor");
        (*env)->DeleteLocalRef(env, detailsClass);
        if (beatGridObj) (*env)->DeleteLocalRef(env, beatGridObj);
        if (phraseArray) (*env)->DeleteLocalRef(env, phraseArray);
        if (cueArray) (*env)->DeleteLocalRef(env, cueArray);
        return NULL;
    }

    jobject details = (*env)->NewObject(env, detailsClass, detailsCtor, song, beatGridObj, NULL, 0, phraseArray, cueArray);

    (*env)->DeleteLocalRef(env, detailsClass);
    if (beatGridObj) (*env)->DeleteLocalRef(env, beatGridObj);
    if (phraseArray) (*env)->DeleteLocalRef(env, phraseArray);
    if (cueArray) (*env)->DeleteLocalRef(env, cueArray);

    LOG("Successfully instantiated RekordboxSongDetails with BeatGrid and %d cues", cue_count);
    return details;
}
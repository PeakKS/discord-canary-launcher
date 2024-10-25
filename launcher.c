#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <curl/curl.h>
#include <json-c/json.h>

#define CANARY_URL "https://discord.com/api/download/canary?platform=linux"
#define CANARY_DIR "/opt/discord-canary"
#define BUILD_INFO CANARY_DIR "/resources/build_info.json"

#define MAX_VERSION_LENGTH 16

int get_local_version (char *version_string, size_t version_string_length) {
  json_object *root = json_object_from_file (BUILD_INFO);
  if (!root)
    return -1;

  json_object *version = json_object_object_get (root, "version");
  if (!version)
    return -2;

  const char *vs = json_object_get_string (version);
  strncpy (version_string, vs, version_string_length);
  json_object_put (root);
  return 0;
}

int get_remote_version (CURL *curl, char *version_string, size_t version_string_length) {
  struct curl_header *location;
  CURLHcode h;
  const char *latest_url;
  const char *version_start;
  const char *version_end;
  size_t version_length;

  curl_easy_setopt (curl, CURLOPT_URL, CANARY_URL);
  curl_easy_setopt (curl, CURLOPT_NOBODY, 1);
  if (curl_easy_perform (curl) != CURLE_OK)
    return -1;
  
  h = curl_easy_header (curl, "location", 0, CURLH_HEADER, -1, &location);
  if (h !=  CURLHE_OK)
    return -2;

  latest_url = location->value;
  version_start = strrchr (latest_url, '-') + 1;
  version_end = strrchr (latest_url, '.');
  version_length = version_end - version_start;

  if (version_string_length < version_length)
    version_length = version_string_length;

  strncpy (version_string, version_start, version_length);
  return 0;
}

int need_update (CURL *curl) {
  char local_version[MAX_VERSION_LENGTH];
  char remote_version[MAX_VERSION_LENGTH];
  int rc;

  rc = get_local_version (local_version, sizeof(local_version));
  if (rc < 0)
    return -1;

  rc = get_remote_version (curl, remote_version, sizeof(remote_version));
  if (rc < 0)
    return -1;

  return strverscmp (remote_version, local_version) > 0;
}

int main(void) {
  unsigned int user;
  CURL *curl;

  user = getuid();
  curl = curl_easy_init();
  if (!curl)
    return -1;

  if (need_update (curl)) {
    printf("Need update!\n");
  }

  setuid (user);
  curl_easy_cleanup (curl);
  return 0;
}
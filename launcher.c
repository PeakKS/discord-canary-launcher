#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <curl/curl.h>
#include <json-c/json.h>
#include <archive.h>
#include <archive_entry.h>
#include <adwaita.h>

#include "config.h"

int
get_local_version (char *version_string, size_t version_string_length) {
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

int
get_remote_version (CURL *curl, char *version_string, size_t version_string_length) {
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
  curl_easy_reset(curl);
  return 0;
}

int
need_update (CURL *curl, char *version, size_t version_length) {
  char local_version[VERSION_MAX_LENGTH];
  char remote_version[VERSION_MAX_LENGTH];
  int rc;

  rc = get_local_version (local_version, sizeof(local_version));
  if (rc < 0)
    return -1;

  rc = get_remote_version (curl, remote_version, sizeof(remote_version));
  if (rc < 0)
    return -1;

  strncpy (version, remote_version, version_length);

  if (strverscmp (remote_version, local_version) > 0) {
    return 1;
  }

  return 0;
}

struct curl_download_memory {
  char *memory;
  size_t size;
};

static size_t
curl_download_memory_callback (void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct curl_download_memory *mem = (struct curl_download_memory *) userp;

  char *ptr = realloc (mem->memory, mem->size + realsize + 1);
  if (!ptr) {
    fprintf (stderr, "realloc: Out of memory!\n");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&mem->memory[mem->size], contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

struct transfer_progress {
  GtkWidget *bar;
  double last_update;
};

static size_t
curl_transfer_callback (void *userp, curl_off_t dltotal, curl_off_t dlnow, 
G_GNUC_UNUSED curl_off_t ultotal, G_GNUC_UNUSED curl_off_t ulnow) {
  struct transfer_progress *progress = userp;
  double ratio = (double)dlnow / (double)dltotal;
  if ((ratio - progress->last_update) > 0.1) {
    printf ("Download progress: (%.1f%%)\n", ratio * 100);
    progress->last_update = ratio;
  }
  if (ratio > 0.0 && ratio < 100.0) {
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress->bar), ratio);
    g_main_context_iteration (NULL, FALSE);
  }
  return 0;
}

int
download (CURL *curl, const char *version, struct curl_download_memory *download, GtkWidget *progressbar) {
  printf("Downloading...\n");
  CURLcode res;
  struct transfer_progress progress;
  char download_url[sizeof(CANARY_DOWNLOAD_URL) + (2 * VERSION_MAX_LENGTH)];

  progress.bar = progressbar;
  progress.last_update = 0.0;

  snprintf(download_url, sizeof(download_url), CANARY_DOWNLOAD_URL, version, version);
  curl_easy_setopt (curl, CURLOPT_URL, download_url);
  curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION, curl_transfer_callback);
  curl_easy_setopt (curl, CURLOPT_XFERINFODATA, &progress);
  curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, curl_download_memory_callback);
  curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *)download);

  res = curl_easy_perform (curl);
  if (res != CURLE_OK) {
    fprintf (stderr, "Download failed: %s\n", curl_easy_strerror (res));
    return -1;
  }

  curl_easy_reset(curl);
  return 0;
}

int
extract (struct curl_download_memory *debpkg) {
  printf("Extracting deb...\n");
  struct archive *a;
  struct archive *writer;
  struct archive_entry *entry;
  size_t datatar_size = 0;
  void *datatar = NULL;
  size_t read_bytes = 0;

  a = archive_read_new ();
  archive_read_support_filter_gzip (a);
  archive_read_support_format_ar (a);

  if (archive_read_open_memory (a, debpkg->memory, debpkg->size) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to read debpkg memory\n");
    return -1;
  }

  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
    if (strcmp ("data.tar.gz", archive_entry_pathname (entry)) == 0) {
      datatar_size = archive_entry_size (entry);
      break;
    }
  }

  if (datatar_size == 0) {
    fprintf (stderr, "Failed to find data.tar.gz in discord deb file\n");
    return -2;
  }

  datatar = malloc (datatar_size);
  read_bytes = archive_read_data (a, datatar, datatar_size);
  if (read_bytes != datatar_size) {
    fprintf (stderr, "data.tar.gz size mismatch: read %ld bytes instead of %ld bytes\n", read_bytes, datatar_size);
    return -3;
  }

  if (archive_read_free (a) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to free debpkg archive\n");
    return -4;
  }

  printf("Extracting data.tar.gz...\n");

  a = archive_read_new ();
  archive_read_support_filter_gzip (a);
  archive_read_support_format_tar (a);

  if (archive_read_open_memory (a, datatar, datatar_size) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to open data.tar.gz as archive\n");
    return -5;
  }

  writer = archive_write_disk_new ();
  size_t cmp_length = strlen(DATATAR_DATA_PREFIX);
  size_t strip_length = strlen(DATATAR_ORIG_PREFIX);
  char newpath[PATH_MAX];
  void *data = NULL;
  size_t data_capacity = 0;
  int rc;

  while ((rc = archive_read_next_header (a, &entry)) == ARCHIVE_OK) {
    const char *pathname;
    size_t data_size;
    
    pathname = archive_entry_pathname (entry);
    data_size = archive_entry_size (entry);

    if (strlen(pathname) < cmp_length)
      continue;
    if (strncmp (pathname, DATATAR_DATA_PREFIX, cmp_length) != 0)
      continue;

    snprintf (newpath, PATH_MAX, "%s%s", DATATAR_INST_PREFIX, &pathname[strip_length]);
    archive_entry_set_pathname (entry, newpath);

    if (data_size > data_capacity) {
      char *ptr = realloc (data, data_size);
      if (!ptr) {
        perror ("Failed to extract archive entry, out of memory");
        exit (1);
      }
      data = ptr;
    }

    read_bytes = archive_read_data (a, data, data_size);
    if (read_bytes != data_size) {
      fprintf (stderr, "Extraction size mismatch %ld:%ld", read_bytes, data_size);
      exit (1);
    }

    int hwrite = archive_write_header (writer, entry);
    if (hwrite != ARCHIVE_OK) {
      fprintf (stderr, "Archive header write error: %s\n", archive_error_string (writer));
      exit (1);
    }
    if (archive_write_data (writer, data, data_size) < (la_ssize_t) data_size) {
      fprintf (stderr, "Failure while writing file %s: %s\n", newpath, archive_error_string (writer));
    }
  }

  if (rc != ARCHIVE_EOF) {
    fprintf (stderr, "Failure while reading archive: %s\n", archive_error_string (writer));
    exit (1);
  }

  free (data);

  if (archive_read_free (a) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to free debpkg archive\n");
    exit (1);
  }

  free (datatar);

  if (archive_write_close (writer) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to close writer: %s\n", archive_error_string (writer));
    exit (1);
  }

  if (archive_write_free (writer) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to free archive writer\n");
    exit (1);
  }
  
  return 0;
}

void
launch_discord (char ** argv) {
  chdir (CANARY_DIR);
  execv (CANARY_EXEC, argv);
}

void
cancel (G_GNUC_UNUSED GtkButton *button, G_GNUC_UNUSED gpointer user_data) {
  exit (1);
}

int
main(int argc, char **argv) {
  g_autoptr (GOptionContext) optctx = NULL;
  GtkWidget *window;
  GtkWidget *dialog;
  GtkWidget *progressbar;
  GError *error = NULL;
  gboolean forceupdate = FALSE;
  gboolean dumpconfig = FALSE;
  CURL *curl;
  char latest_version[VERSION_MAX_LENGTH];
  struct curl_download_memory debpkg;
  debpkg.memory = malloc(1);
  debpkg.size = 0;

  GOptionEntry entries[] = {
    { "forceupdate", 'f', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &forceupdate, "Force an update", NULL },
    { "dumpconfig", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &dumpconfig, "Dump compiled-in config", NULL },
    G_OPTION_ENTRY_NULL
  };

  optctx = g_option_context_new (" -- an automatic updater for Discord Canary");
  g_option_context_add_main_entries (optctx, entries, NULL);
  g_option_context_parse (optctx, &argc, &argv, &error);

  if (dumpconfig) {
    puts (
      "Compiled Configuration:\n"
      "\tCanary URL: " CANARY_URL "\n"
      "\tCanary Download URL: " CANARY_DOWNLOAD_URL "\n"
      "\tCanary Install Prefix: " CANARY_DIR "\n"
    );
  }

  adw_init ();

  window = adw_window_new ();
  gtk_window_set_title (GTK_WINDOW (window), "Discord Canary Updater");
  gtk_window_set_default_size (GTK_WINDOW (window), 300, 100);

  dialog = adw_message_dialog_new (GTK_WINDOW (window), "Discord Canary Updater", "Checking for update...");
  adw_message_dialog_add_response (ADW_MESSAGE_DIALOG (dialog), "cancel", "Cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (cancel), NULL);

  if (access (BUILD_INFO, W_OK) < 0) {
    perror ("You do not have permission to update Discord's files");

    adw_message_dialog_set_body (ADW_MESSAGE_DIALOG (dialog), "You do not have permission to update Discord's files");
    gtk_window_present (GTK_WINDOW (dialog));
    while (g_main_context_iteration (NULL, TRUE));

    exit (1);
  }

  progressbar = gtk_progress_bar_new ();
  adw_message_dialog_set_extra_child (ADW_MESSAGE_DIALOG (dialog), progressbar);

  gtk_window_present (GTK_WINDOW (dialog));
  g_main_context_iteration (NULL, TRUE);

  curl = curl_easy_init();
  if (!curl)
    exit (1);

  if ((need_update (curl, latest_version, VERSION_MAX_LENGTH) > 0) || forceupdate) {
    printf("Need update!\n");

    adw_message_dialog_set_body (ADW_MESSAGE_DIALOG (dialog), "Downloading update...");
    g_main_context_iteration (NULL, TRUE);
    if (download (curl, latest_version, &debpkg, progressbar) < 0)
      exit (1);

    adw_message_dialog_set_body (ADW_MESSAGE_DIALOG (dialog), "Extracting...");
    g_main_context_iteration (NULL, TRUE);
    if (extract (&debpkg) < 0)
      exit (1);
      
  } else {
    printf("Up to date!\n");
  }

  free (debpkg.memory);
  debpkg.memory = NULL;
  debpkg.size = 0;
  curl_easy_cleanup (curl);

  launch_discord (argv);
  return 0;
}
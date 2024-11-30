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

struct options {
  bool forceupdate;
};

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
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress->bar), ratio);
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
  while (archive_read_next_header (a, &entry) == ARCHIVE_OK) {
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
    archive_entry_set_uid (entry, 0);
    archive_entry_set_gid (entry, 0);

    if (data_size > data_capacity) {
      char *ptr = realloc (data, data_size);
      if (!ptr) {
        fprintf (stderr, "realloc: Out of memory!\n");
        return -6;
      }
      data = ptr;
    }

    read_bytes = archive_read_data (a, data, data_size);
    if (read_bytes != data_size) {
      fprintf (stderr, "Extraction size mismatch %ld:%ld", read_bytes, data_size);
      return -6;
    }

    int hwrite = archive_write_header (writer, entry);
    if (hwrite != ARCHIVE_OK) {
      fprintf (stderr, "Archive header write error: %s\n", archive_error_string (writer));
    }
    archive_write_data (writer, data, data_size);
  }

  free (data);

  if (archive_read_free (a) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to free debpkg archive\n");
    return -7;
  }

  free (datatar);

  if (archive_write_free (writer) != ARCHIVE_OK) {
    fprintf (stderr, "Failed to free archive writer\n");
    return -8;
  }
  
  return 0;
}

void
launch_discord (unsigned int user, unsigned int group, char ** argv) {
  setuid (user);
  setgid (group);
  chdir (CANARY_DIR);
  execv(CANARY_EXEC, argv);
}

static void
activate (GtkApplication *app, gpointer user_data) {
  GtkWidget *window;
  GtkWidget *box;
  GtkWidget *progress;
  CURL *curl;
  char latest_version[VERSION_MAX_LENGTH];
  struct curl_download_memory debpkg;
  debpkg.memory = malloc(1);
  debpkg.size = 0;

  struct options *app_options = user_data;

  window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "Discord Canary Launcher");
  gtk_window_set_default_size (GTK_WINDOW (window), 200, 100);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_window_set_child (GTK_WINDOW (window), box);

  progress = gtk_progress_bar_new ();
  gtk_progress_bar_get_show_text (GTK_PROGRESS_BAR (progress));
  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (progress), "Checking for update...");
  gtk_box_append (GTK_BOX (box), progress);

  gtk_window_present (GTK_WINDOW (window));

  curl = curl_easy_init();
  if (!curl)
    return;

  if ((need_update (curl, latest_version, VERSION_MAX_LENGTH) > 0) || app_options->forceupdate) {
    printf("Need update!\n");
  } else {
    printf("Up to date!\n");
    goto finish;
  }

  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (progress), "Downloading update...");
  if (download (curl, latest_version, &debpkg, progress) < 0) {
    return;
  }

  gtk_progress_bar_set_text (GTK_PROGRESS_BAR (progress), "Extracting...");
  if (extract (&debpkg) < 0) {
    return;
  }
finish:
  free (debpkg.memory);
  debpkg.memory = NULL;
  debpkg.size = 0;
  curl_easy_cleanup (curl);
}

static gint
handle_local_options (G_GNUC_UNUSED GApplication *application,
                      GVariantDict *options,
                      gpointer user_data) {
  struct options *app_options = user_data;
  gboolean dump;

  if (g_variant_dict_lookup (options, "forceupdate", "b", &app_options->forceupdate))
    printf ("Forcing update: %d\n", app_options->forceupdate);
  if (g_variant_dict_lookup (options, "dumpconfig", "b", &dump)) {
    puts (
      "Compiled Configuration:\n"
      "\tCanary URL: " CANARY_URL "\n"
      "\tCanary Download URL: " CANARY_DOWNLOAD_URL "\n"
      "\tCanary Install Prefix: " CANARY_DIR "\n"
    );
  }
  return -1;
}

int
main(int argc, char **argv) {
  unsigned int uid;
  unsigned int gid;
  g_autoptr (AdwApplication) app = NULL;
  int status;
  struct options app_options = {
    .forceupdate = false,
  };

  uid = getuid();
  gid = getgid();
  /*
  if (geteuid () != 0) {
    fprintf (stderr, "Effective UID must be root, set the SUID bit and give ownership to root\n");
    exit(1);
  }
  */

  GOptionEntry entries[] = {
    { "forceupdate", 'f', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Force an update", NULL },
    { "dumpconfig", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Dump compiled-in config", NULL },
    G_OPTION_ENTRY_NULL
  };

  app = adw_application_new ("com.discordapp.launcher", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), &app_options);

  g_application_add_main_option_entries (G_APPLICATION(app), entries);
  g_signal_connect (app, "handle-local-options", G_CALLBACK (handle_local_options), &app_options);

  status = g_application_run (G_APPLICATION (app), argc, argv);

  //launch_discord (uid, gid, argv);
  return status;
}
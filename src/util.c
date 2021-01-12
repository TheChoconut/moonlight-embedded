/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "util.h"
#include "logging.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <pwd.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>


char* get_path(char* name, char* extra_data_dirs) {
  const char *xdg_config_dir = getenv("XDG_CONFIG_DIR");
  const char *home_dir = getenv("HOME");

  if (access(name, R_OK) != -1) {
      return name;
  }

  if (!home_dir) {
    struct passwd *pw = getpwuid(getuid());
    home_dir = pw->pw_dir;
  }

  if (!extra_data_dirs)
    extra_data_dirs = "/usr/share:/usr/local/share";
  if (!xdg_config_dir)
    xdg_config_dir = home_dir;

  char *data_dirs = malloc(strlen(USER_PATHS) + 1 + strlen(xdg_config_dir) + 1 + strlen(home_dir) + 1 + strlen(DEFAULT_CONFIG_DIR) + 1 + strlen(extra_data_dirs) + 2);
  sprintf(data_dirs, USER_PATHS ":%s:%s/" DEFAULT_CONFIG_DIR ":%s/", xdg_config_dir, home_dir, extra_data_dirs);

  char *path = malloc(strlen(data_dirs)+strlen(MOONLIGHT_PATH)+strlen(name)+2);
  if (path == NULL) {
    _moonlight_log(ERR, "Not enough memory\n");
    exit(-1);
  }

  char* data_dir = data_dirs;
  char* end;
  do {
    end = strstr(data_dir, ":");
    int length = end != NULL ? end - data_dir:strlen(data_dir);
    memcpy(path, data_dir, length);
    if (path[0] == '/')
      sprintf(path+length, MOONLIGHT_PATH "/%s", name);
    else
      sprintf(path+length, "/%s", name);

    if(access(path, R_OK) != -1) {
      free(data_dirs);
      return path;
    }

    data_dir = end + 1;
  } while (end != NULL);

  free(data_dirs);
  free(path);
  return NULL;
}

int set_bool(char *path, bool value) {
  int fd = open(path, O_RDWR);

  if(fd >= 0) {
    int ret = write(fd, value ? "1" : "0", 1);
    if (ret < 0)
      _moonlight_log(ERR, "Failed to set boolean parameter %s, error %d!\n", path, ret);

    close(fd);
    return 0;
  } else
    return -1;
}

int set_int(char *path, int value) {
  int fd = open(path, O_RDWR);
  if(fd >= 0) {

    int length = snprintf( NULL, 0, "%d", value );
    char* str = malloc( length + 1 );
    snprintf( str, length + 1, "%d", value );
    int ret = write(fd, str, length);
    if (ret < 0)
      _moonlight_log(ERR, "Failed to set int parameter %s to: %s, error %d!\n", path, str, ret);

    free(str);
    close(fd);
    return 0;
  } else
    return -1;
}
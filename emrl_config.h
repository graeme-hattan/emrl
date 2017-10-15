/*
 * emrl_config.h -- emrl line editing library configuration
 *
 * Copyright (C) 2017 Graeme Hattan (graemeh.dev@gmail.com)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef EMRL_CONFIG_H
#define EMRL_CONFIG_H

#include <stdio.h>

#define EMRL_MAX_CMD_LEN 127

#define USE_INSERT_ESCAPE_SEQUENCE
#define USE_DELETE_ESCAPE_SEQUENCE

typedef FILE* emrl_file;

#endif

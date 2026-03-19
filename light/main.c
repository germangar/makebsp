/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../common/cmdlib.h"
#include <stdio.h>
#include <string.h>

int LightMain(int argc, char **argv);
int VLightMain(int argc, char **argv);
int VSoundMain(int argc, char **argv);

int main(int argc, char **argv) {
  _printf("Q3Light v1.0 (c) 1999 Id Software Inc.\n");

  if (argc < 2) {
    Error("usage: light [options] bspfile\n"
          "Standard lighting: light -light [options] bspfile\n"
          "Vertex lighting:   light -vlight [options] bspfile\n"
          "Sound lighting:    light -vsound [options] bspfile\n");
  }

  if (!strcmp(argv[1], "-light")) {
    return LightMain(argc - 1, argv + 1);
  }
  if (!strcmp(argv[1], "-vlight")) {
    return VLightMain(argc - 1, argv + 1);
  }
  if (!strcmp(argv[1], "-vsound")) {
    return VSoundMain(argc - 1, argv + 1);
  }

  // Default to -light if no recognizable flag is provided
  return LightMain(argc, argv);
}

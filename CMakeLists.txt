# Hively Playback Plugin
#
# Plays AHX and HVL (Hively Tracker) music formats.
# Based on the hvl_replay library by Xeron/IRIS.

cmake_minimum_required(VERSION 3.16)
project(playback_hively C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

add_library(hively_playback SHARED hively_plugin.c hvl_replay.c)
rv_set_plugin_properties(hively_playback)

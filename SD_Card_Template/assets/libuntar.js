// Copyright (c) 2015 Matthew Brennan Jones <matthew.brennan.jones@gmail.com>
// This software is licensed under a MIT License
// https://github.com/workhorsy/uncompress.js

"use strict";

// Based on the information from:
// https://en.wikipedia.org/wiki/Tar_(computing)


(function() {

var TAR_TYPE_FILE = 0;
var TAR_TYPE_DIR = 5;

var TAR_HEADER_SIZE = 512;
var TAR_TYPE_OFFSET = 156;
var TAR_TYPE_SIZE = 1;
var TAR_SIZE_OFFSET = 124;
var TAR_SIZE_SIZE = 12;
var TAR_NAME_OFFSET = 0;
var TAR_NAME_SIZE = 100;

function _tarRead(view, offset, size) {
	return view.slice(offset, offset + size);
}

function tarGetEntries(filename, array_buffer) {
	var view = new Uint8Array(array_buffer);
	var offset = 0;
	var entries = [];

	while (offset + TAR_HEADER_SIZE < view.byteLength) {
		// Get entry name
		var entry_name = saneMap(_tarRead(view, offset + TAR_NAME_OFFSET, TAR_NAME_SIZE), String.fromCharCode);
		entry_name = entry_name.join('').replace(/\0/g, '');

		// No entry name, so probably the last block
		if (entry_name.length === 0) {
			break;
		}

		// Get entry size
		var entry_size = parseInt(saneJoin(saneMap(_tarRead(view, offset + TAR_SIZE_OFFSET, TAR_SIZE_SIZE), String.fromCharCode), ''), 8);
		var entry_type = saneMap(_tarRead(view, offset + TAR_TYPE_OFFSET, TAR_TYPE_SIZE), String.fromCharCode) | 0;

		// Save this as en entry if it is a file or directory
		if (entry_type === TAR_TYPE_FILE || entry_type === TAR_TYPE_DIR) {
			var entry = {
				name: entry_name,
				size: entry_size,
				is_file: entry_type == TAR_TYPE_FILE,
				offset: offset
			};
			entries.push(entry);
		}

		// Round the offset up to be divisible by TAR_HEADER_SIZE
		offset += (entry_size + TAR_HEADER_SIZE);
		if (offset % TAR_HEADER_SIZE > 0) {
			var even = (offset / TAR_HEADER_SIZE) | 0; // number of times it goes evenly into TAR_HEADER_SIZE
			offset = (even + 1) * TAR_HEADER_SIZE;
		}
	}

	return entries;
}

function tarGetEntryData(entry, array_buffer) {
	var view = new Uint8Array(array_buffer);
	var offset = entry.offset;
	var size = entry.size;

	// Get entry data
	var entry_data = _tarRead(view, offset + TAR_HEADER_SIZE, size);
	return entry_data;
}

// Figure out if we are running in a Window or Web Worker
var scope = null;
if (typeof window === 'object') {
	scope = window;
} else if (typeof importScripts === 'function') {
	scope = self;
}

// Set exports
scope.tarGetEntries = tarGetEntries;
scope.tarGetEntryData = tarGetEntryData;
})();

// Copyright (c) 2015 Matthew Brennan Jones <matthew.brennan.jones@gmail.com>
// This software is licensed under a MIT License
// https://github.com/workhorsy/uncompress.js
"use strict";

function loadScript(url) {
	if (typeof window === 'object') {
		var script = document.createElement('script');
		script.type = "text/javascript";
		script.src = url;
		document.head.appendChild(script);
	} else if (typeof importScripts === 'function') {
		importScripts(url);
	}
}

function currentScriptPath() {
	try {
		throw new Error('');
	} catch(e) {
		var stack = e.stack;
		var line = null;
		if (stack.indexOf('@') !== -1) {
			line = stack.split('@')[1].split('\n')[0];
		} else {
			line = stack.split('(')[1].split(')')[0];
		}
		line = line.substring(0, line.lastIndexOf('/')) + '/';
		return line;
	}
}

var unrarMemoryFileLocation = null;

(function() {
	var _loaded_archive_formats = [];

	if (typeof Uint8Array !== 'undefined') {
		if (! Uint8Array.prototype.slice) {
			Uint8Array.prototype.slice = function(start, end) {
				var retval = new Uint8Array(end - start);
				var j = 0;
				for (var i=start; i<end; ++i) {
					retval[j] = this[i];
					++j;
				}
				return retval;
			};
		}
	}

	function saneJoin(array, separator) {
		var retval = '';
		for (var i=0; i<array.length; ++i) {
			if (i === 0) {
				retval += array[i];
			} else {
				retval += separator + array[i];
			}
		}
		return retval;
	}

	function saneMap(array, cb) {
		var retval = new Array(array.length);
		for (var i=0; i<array.length; ++i) {
			retval[i] = cb(array[i]);
		}
		return retval;
	}

	function loadArchiveFormats(formats) {
		var script_path = currentScriptPath();
		formats.forEach(function(format) {
			if (_loaded_archive_formats.indexOf(format) !== -1) return;
			_loaded_archive_formats.push(format);

			if (format === 'rar') {
				unrarMemoryFileLocation = script_path + 'libunrar.js.mem';
				loadScript(script_path + 'libunrar.js');
			} else if (format === 'zip') {
				loadScript(script_path + 'jszip.js');
			} else if (format === 'tar') {
				loadScript(script_path + 'libuntar.js');
			} else {
				throw new Error('Unknown archive format: ' + format);
			}
		});
	}

	function archiveOpenFile(file, cb) {
		var file_name = file.name;
		var reader = new FileReader();
		reader.onload = function(event) {
			var array_buffer = event.target.result;
			archiveOpenArrayBuffer(file_name, array_buffer, cb);
		};
		reader.readAsArrayBuffer(file);
	}

	function archiveOpenArrayBuffer(file_name, array_buffer, cb) {
		setTimeout(function() {
			try {
				var archive = _archiveOpen(file_name, array_buffer);
				cb(archive, null);
			} catch(err) {
				cb(null, err);
			}
		}, 0);
	}

	function _archiveOpen(file_name, array_buffer) {
		var archive_type = null;
		var handle = null;
		var entries = null;

		if (isZipFile(array_buffer)) {
			archive_type = 'zip';
			handle = _zipOpen(file_name, array_buffer);
			entries = _zipGetEntries(handle);
		} else if (isRarFile(array_buffer)) {
			archive_type = 'rar';
			handle = _rarOpen(file_name, array_buffer);
			entries = _rarGetEntries(handle);
		} else if (isTarFile(array_buffer)) {
			archive_type = 'tar';
			handle = _tarOpen(file_name, array_buffer);
			entries = _tarGetEntries(handle);
		} else {
			throw new Error('Unknown archive type');
		}

		entries.sort(function(a, b) {
			if (a.name > b.name) return 1;
			if (a.name < b.name) return -1;
			return 0;
		});

		return {
			file_name: file_name,
			archive_type: archive_type,
			array_buffer: array_buffer,
			entries: entries,
			handle: handle
		};
	}

	function archiveClose(archive) {
		archive.file_name = null;
		archive.archive_type = null;
		archive.array_buffer = null;
		archive.entries = null;
		archive.handle = null;
	}

	function _rarOpen(file_name, array_buffer) {
		var rar_files = [{
			name: file_name,
			size: array_buffer.byteLength,
			type: '',
			content: new Uint8Array(array_buffer)
		}];
		return {
			file_name: file_name,
			array_buffer: array_buffer,
			password: null,
			rar_files: rar_files
		};
	}

	function _zipOpen(file_name, array_buffer) {
		var zip = new JSZip(array_buffer);
		return {
			file_name: file_name,
			array_buffer: array_buffer,
			password: null,
			zip: zip
		};
	}

	function _tarOpen(file_name, array_buffer) {
		return {
			file_name: file_name,
			array_buffer: array_buffer,
			password: null
		};
	}

	function _rarGetEntries(rar_handle) {
		var info = readRARFileNames(rar_handle.rar_files, rar_handle.password);
		var entries = [];
		Object.keys(info).forEach(function(i) {
			var name = info[i].name;
			var is_file = info[i].is_file;
			entries.push({
				name: name,
				is_file: info[i].is_file,
				size_compressed: info[i].size_compressed,
				size_uncompressed: info[i].size_uncompressed,
				readData: function(cb) {
					setTimeout(function() {
						if (is_file) {
							try {
								readRARContent(rar_handle.rar_files, rar_handle.password, name, cb);
							} catch (e) {
								cb(null, e);
							}
						} else {
							cb(null, null);
						}
					}, 0);
				}
			});
		});
		return entries;
	}

	function _zipGetEntries(zip_handle) {
		var zip = zip_handle.zip;
		var entries = [];
		Object.keys(zip.files).forEach(function(i) {
			var zip_entry = zip.files[i];
			var name = zip_entry.name;
			var is_file = ! zip_entry.dir;
			var size_compressed = zip_entry._data ? zip_entry._data.compressedSize : 0;
			var size_uncompressed = zip_entry._data ? zip_entry._data.uncompressedSize : 0;
			entries.push({
				name: name,
				is_file: is_file,
				size_compressed: size_compressed,
				size_uncompressed: size_uncompressed,
				readData: function(cb) {
					setTimeout(function() {
						if (is_file) {
							var data = zip_entry.asArrayBuffer();
							cb(data, null);
						} else {
							cb(null, null);
						}
					}, 0);
				}
			});
		});
		return entries;
	}

	function _tarGetEntries(tar_handle) {
		var tar_entries = tarGetEntries(tar_handle.file_name, tar_handle.array_buffer);
		var entries = [];
		tar_entries.forEach(function(entry) {
			var name = entry.name;
			var is_file = entry.is_file;
			var size = entry.size;
			entries.push({
				name: name,
				is_file: is_file,
				size_compressed: size,
				size_uncompressed: size,
				readData: function(cb) {
					setTimeout(function() {
						if (is_file) {
							var data = tarGetEntryData(entry, tar_handle.array_buffer);
							cb(data.buffer, null);
						} else {
							cb(null, null);
						}
					}, 0);
				}
			});
		});
		return entries;
	}

	function isRarFile(array_buffer) {
		var rar_header1 = saneJoin([0x52, 0x45, 0x7E, 0x5E], ', ');
		var rar_header2 = saneJoin([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00], ', ');
		var rar_header3 = saneJoin([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00], ', ');
		if (array_buffer.byteLength < 8) return false;
		var header1 = saneJoin(new Uint8Array(array_buffer).slice(0, 4), ', ');
		var header2 = saneJoin(new Uint8Array(array_buffer).slice(0, 7), ', ');
		var header3 = saneJoin(new Uint8Array(array_buffer).slice(0, 8), ', ');
		return (header1 === rar_header1 || header2 === rar_header2 || header3 === rar_header3);
	}

	function isZipFile(array_buffer) {
		var zip_header = saneJoin([0x50, 0x4b, 0x03, 0x04], ', ');
		if (array_buffer.byteLength < 4) return false;
		var header = saneJoin(new Uint8Array(array_buffer).slice(0, 4), ', ');
		return (header === zip_header);
	}

	function isTarFile(array_buffer) {
		var tar_header = saneJoin(['u', 's', 't', 'a', 'r'], ', ');
		if (array_buffer.byteLength < 512) return false;
		var header = saneJoin(saneMap(new Uint8Array(array_buffer).slice(257, 257 + 5), String.fromCharCode), ', ');
		return (header === tar_header);
	}

	var scope = null;
	if (typeof window === 'object') {
		scope = window;
	} else if (typeof importScripts === 'function') {
		scope = self;
	}

	scope.loadArchiveFormats = loadArchiveFormats;
	scope.archiveOpenFile = archiveOpenFile;
	scope.archiveOpenArrayBuffer = archiveOpenArrayBuffer;
	scope.archiveClose = archiveClose;
	scope.isRarFile = isRarFile;
	scope.isZipFile = isZipFile;
	scope.isTarFile = isTarFile;
	scope.saneJoin = saneJoin;
	scope.saneMap = saneMap;
})();

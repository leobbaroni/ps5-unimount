//! Adapts Kraken Mount's plaintext package reader and index writer to SMP.
#![cfg_attr(not(test), no_std)]

extern crate alloc;

#[path = "../../vendor/kraken/src/ampr_index.rs"]
mod ampr_index;
mod lih;

use alloc::{collections::BTreeMap, format, string::String, vec, vec::Vec};
use ampr_index::{Chunk, IndexFile, Kraken};
use core::{ffi::c_void, ptr, slice, str};
#[cfg(not(test))]
use core::{alloc::{GlobalAlloc, Layout}, panic::PanicInfo};
use kraken_mount_pkg::{Block, Entry, Error, PRead, Pkg};

const COPY_BUFFER: usize = 1024 * 1024;
const REQUIRED_FILES: [&str; 3] = ["eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png"];

#[cfg(not(test))]
extern "C" {
    fn posix_memalign(out: *mut *mut u8, alignment: usize, size: usize) -> i32;
    fn free(ptr: *mut u8);
    fn abort() -> !;
}

#[cfg(not(test))]
struct Allocator;

#[cfg(not(test))]
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let mut result = ptr::null_mut();
        let alignment = layout.align().max(core::mem::size_of::<usize>());
        if posix_memalign(&mut result, alignment, layout.size()) != 0 {
            return ptr::null_mut();
        }
        result
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr);
    }
}

#[cfg(not(test))]
#[global_allocator]
static ALLOCATOR: Allocator = Allocator;

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    unsafe { abort() }
}

// The prebuilt core references this symbol in its unwind metadata.
// The bridge uses panic=abort and never unwinds across the C boundary.
#[cfg(not(test))]
#[no_mangle]
extern "C" fn rust_eh_personality() -> ! {
    unsafe { abort() }
}

#[cfg(test)]
#[path = "sparse_tests.rs"]
mod sparse_tests;

type Read = unsafe extern "C" fn(*mut c_void, u64, *mut u8, usize) -> isize;
type Write = unsafe extern "C" fn(*mut c_void, *const u8, usize, u64, *const u8, usize) -> i32;
type LooseSize = unsafe extern "C" fn(*mut c_void, *const u8, usize, *mut u64) -> i32;
type Progress = unsafe extern "C" fn(*mut c_void, u32, u64, u64);
type LoadVerification = unsafe extern "C" fn(*mut c_void, *mut u8, usize) -> bool;
type SaveVerification = unsafe extern "C" fn(*mut c_void, *const u8, usize);

#[derive(Debug)]
enum BuildError {
    Other(String),
    Decode(String),
}

impl From<String> for BuildError {
    fn from(message: String) -> Self {
        Self::Other(message)
    }
}

impl From<Error> for BuildError {
    fn from(error: Error) -> Self {
        Self::from_package(error, None)
    }
}

impl BuildError {
    fn from_package(error: Error, path: Option<&str>) -> Self {
        let message = match path {
            Some(path) => format!("{path}: {error}"),
            None => format!("{error}"),
        };
        match error {
            Error::Decode(_) => Self::Decode(message),
            _ => Self::Other(message),
        }
    }

    fn file(path: &str, error: Error) -> Self {
        Self::from_package(error, Some(path))
    }
}

struct PackageIo {
    context: *mut c_void,
    read: Read,
    write: Write,
    loose_size: LooseSize,
}

impl PRead for PackageIo {
    fn pread(&self, offset: u64, buf: &mut [u8]) -> Result<usize, kraken_mount_pkg::Error> {
        CallbackReader { context: self.context, read: self.read }.pread(offset, buf)
    }
}

struct CallbackReader {
    context: *mut c_void,
    read: Read,
}

impl PRead for CallbackReader {
    fn pread(&self, offset: u64, buf: &mut [u8]) -> Result<usize, Error> {
        let count = unsafe { (self.read)(self.context, offset, buf.as_mut_ptr(), buf.len()) };
        if count < 0 || count as usize > buf.len() {
            return Err(Error::Io(count.saturating_neg() as i32));
        }
        Ok(count as usize)
    }
}

impl PackageIo {
    fn output(&self, path: &str, offset: u64, data: &[u8]) -> Result<(), String> {
        let status = unsafe {
            (self.write)(self.context, path.as_ptr(), path.len(), offset, data.as_ptr(), data.len())
        };
        if status == 0 { Ok(()) } else { Err(format!("write {path}: errno {status}")) }
    }

    fn override_size(&self, path: &str) -> Result<Option<u64>, String> {
        let mut size = 0;
        let status = unsafe { (self.loose_size)(self.context, path.as_ptr(), path.len(), &mut size) };
        match status {
            0 => Ok(None),
            1 => Ok(Some(size)),
            code => Err(format!("read staged {path}: errno {}", -code)),
        }
    }

    fn extract(&self, reader: &impl PRead, pkg: &Pkg, entry: &Entry<'_>, path: &str,
               buf: &mut [u8]) -> Result<(), BuildError> {
        if entry.size == 0 {
            return self.output(path, 0, &[]).map_err(Into::into);
        }
        let sparse = pkg.has_sparse(entry);
        let mut offset = 0;
        while offset < entry.size {
            let length = pkg.read_at(reader, entry, offset, buf).map_err(|error| BuildError::file(path, error))?;
            if length == 0 || length as u64 > entry.size - offset {
                return Err(format!("{path}: short package read").into());
            }
            if sparse && buf[..length].iter().all(|&byte| byte == 0) {
                if offset + length as u64 == entry.size {
                    self.output(path, entry.size - 1, &[0])?;
                }
            } else {
                self.output(path, offset, &buf[..length])?;
            }
            offset += length as u64;
        }
        Ok(())
    }
}

fn original_chunk(block: &Block, offset: u64) -> Chunk {
    Chunk {
        pkg_off: offset,
        stored_len: block.stored_len,
        raw_len: block.raw_len,
        staged: false,
        kraken: block.compressed.then_some(Kraken { even_len: block.desc.even_len, mode: block.desc.mode }),
    }
}

struct PatchCache<'a> {
    io: &'a PackageIo,
    end: u64,
}

impl PatchCache<'_> {
    fn append(&mut self, bytes: &[u8]) -> Result<Chunk, String> {
        let offset = self.end;
        self.end += (bytes.len() as u64).next_multiple_of(ampr_index::PAGE);
        let len = bytes.len() as u32;
        self.io.output(ampr_index::PATCH_PACK_NAME, offset, bytes)?;
        if bytes.len() as u64 % ampr_index::PAGE != 0 {
            self.io.output(ampr_index::PATCH_PACK_NAME, self.end - 1, &[0])?;
        }
        Ok(Chunk { pkg_off: offset, stored_len: len, raw_len: len, staged: true, kraken: None })
    }
}

fn prepare(io: &PackageIo, size: u64, mtime: i64, basename: &str) -> Result<(), BuildError> {
    let pkg = Pkg::open(io, size)?;
    prepare_package(io, io, &pkg, size, mtime, basename, |_, _| {}, |_, entry, _, _| {
        let blocks = match pkg.blocks(entry) {
            Ok(blocks) => blocks,
            Err(Error::Unsupported(_)) => return Ok(None),
            Err(error) => return Err(error.into()),
        };
        let chunks: Vec<_> = blocks.iter().map(|block| original_chunk(block, block.pkg_off)).collect();
        Ok(ampr_index::packable(entry.size, &chunks, size).then_some(chunks))
    })
}

fn prepare_patched(io: &PackageIo, patch: &impl PRead, size: u64, patch_size: u64, mtime: i64,
                   basename: &str, verification: &lih::Verification<'_>, progress: impl Fn(u32, u64, u64),
                   cancelled: impl Fn() -> bool) -> Result<(), BuildError> {
    let view = lih::View::open(io, size, patch, patch_size, verification, &cancelled,
                               |done, total| progress(0, done, total))?;
    if !verification.fast {
        view.verify_base(|done, total| progress(1, done, total))?;
    }
    let pkg = Pkg::open_updated_image(&view, view.len(), |done, total| progress(3, done, total))?;
    prepare_package(io, &view, &pkg, size, mtime, basename,
                    |done, total| progress(2, done, total), |pkg, entry, cache, buffer| {
        if cancelled() {
            return Err(String::from("LIH cache build cancelled").into());
        }
        let mut blocks = pkg.blocks(entry).unwrap_or_default().iter();
        let mut block = blocks.next();
        let mut block_offset = 0;
        let mut chunks = Vec::new();
        for offset in (0..entry.size).step_by(ampr_index::BLOCK as usize) {
            let want = (entry.size - offset).min(ampr_index::BLOCK);
            while let Some(current) = block {
                let end = block_offset + u64::from(current.raw_len);
                if end > offset {
                    break;
                }
                block_offset = end;
                block = blocks.next();
            }
            if let Some(block) = block.filter(|_| block_offset == offset) {
                let raw_len = u64::from(block.raw_len);
                // Only an exact 256 KiB block can be decoded whole and sliced by the runtime.
                let reusable_tail = !block.compressed ||
                    (raw_len == ampr_index::BLOCK && pkg.has_exact_block_starts());
                if raw_len == want || (raw_len > want && reusable_tail) {
                    if block.stored_len == 0 {
                        chunks.push(original_chunk(block, 0));
                        continue;
                    }
                    let run = view.locate(block.pkg_off).map_err(|error| BuildError::file(entry.path, error))?;
                    if run.source == lih::Source::Base && run.len >= u64::from(block.stored_len) {
                        chunks.push(original_chunk(block, run.offset));
                        continue;
                    }
                }
            }
            let buffer = &mut buffer[..want as usize];
            let count = pkg.read_at(&view, entry, offset, buffer)
                .map_err(|error| BuildError::file(entry.path, error))?;
            if count != buffer.len() {
                return Err(format!("{}: short LIH file read", entry.path).into());
            }
            if buffer.iter().all(|&byte| byte == 0) {
                chunks.push(Chunk { pkg_off: 0, stored_len: 0, raw_len: count as u32, staged: false, kraken: None });
            } else {
                chunks.push(cache.append(buffer)?);
            }
        }
        Ok(Some(chunks))
    })?;
    view.save_verification(verification);
    Ok(())
}

fn prepare_package(
    io: &PackageIo, reader: &impl PRead, pkg: &Pkg, size: u64, mtime: i64, basename: &str,
    progress: impl Fn(u64, u64),
    mut chunks_for_file: impl FnMut(&Pkg, &Entry<'_>, &mut PatchCache<'_>, &mut [u8]) -> Result<Option<Vec<Chunk>>, BuildError>,
) -> Result<(), BuildError> {
    for path in REQUIRED_FILES {
        if !pkg.lookup(path).is_some_and(|entry| !entry.is_dir && entry.size > 0) {
            return Err(format!("base game file {path} is missing").into());
        }
    }
    let mut buffer = vec![0; COPY_BUFFER];
    let mut files = BTreeMap::new();
    let mut cache = PatchCache { io, end: ampr_index::PAGE };
    let total = pkg.files().filter(|entry| !entry.is_dir).count() as u64;
    let primary_lib = if pkg.files().any(|f| !f.is_dir && f.path.to_ascii_lowercase().starts_with("fakelib2/")) {
        "fakelib2/"
    } else {
        "fakelib/"
    };
    for (i, entry) in pkg.files().filter(|f| !f.is_dir).enumerate() {
        progress(i as u64, total);
        ampr_index::check_path(entry.path)?;
        let lower = entry.path.to_ascii_lowercase();
        if let Some(name) = lower.strip_prefix(primary_lib) {
            // Only the game's legacy fakelib can shadow the system save
            // module unintentionally. Keep explicit fakelib2 and backports.
            if primary_lib == "fakelib/" && name == "libscesavedata.native.sprx" {
                continue;
            }
            if !name.contains('/') && !matches!(name, "libkernel.sprx" | "libsceampr.sprx") {
                let name = &entry.path[primary_lib.len()..];
                let out = format!("fakelib2/{name}");
                if io.override_size(&out)?.is_none() {
                    io.extract(reader, pkg, &entry, &out, &mut buffer)?;
                }
            }
            continue;
        }
        if lower.starts_with("fakelib/") || lower.starts_with("fakelib2/") || lower == "ampr_emu.index" {
            continue;
        }
        if matches!(lower.as_str(), "ampr_assets.index" | "ampr_zstd.index") {
            return Err(format!("package contains {lower}; nested AMPR packs are unsupported").into());
        }
        if lower == "kraken" || lower.starts_with("kraken/") || lower == "kraken.image" || lower.starts_with(".smp-kraken-") {
            return Err(format!("reserved cache path {}", entry.path).into());
        }
        if files.contains_key(&lower) {
            return Err(format!("case-insensitive duplicate {}", entry.path).into());
        }
        let loose = io.override_size(entry.path)?;
        let extract = lower.starts_with("sce_sys/") ||
            matches!(lower.as_str(), "eboot.bin" | "eboot2.bin" | "dlc_emu.ini" | "playgo_stub.dat") ||
            lower.ends_with(".prx") || lower.ends_with(".sprx");
        let (length, chunks) = if let Some(length) = loose {
            (length, None)
        } else {
            let chunks = if extract { None } else { chunks_for_file(pkg, &entry, &mut cache, &mut buffer)? };
            if chunks.is_none() {
                io.extract(reader, pkg, &entry, entry.path, &mut buffer)?;
            }
            (entry.size, chunks)
        };
        files.insert(lower, IndexFile { path: String::from(entry.path), size: length, chunks });
    }
    let files: Vec<_> = files.into_values().collect();
    let images = ampr_index::build(size, mtime, &files, cache.end)?;
    if let Some((name, bytes)) = images.zero_pack {
        io.output(name, 0, &bytes)?;
    }
    if let Some((header, size)) = images.patch_volume {
        if size > cache.end {
            io.output(ampr_index::PATCH_PACK_NAME, size - 1, &[0])?;
        }
        io.output(ampr_index::PATCH_PACK_NAME, 0, &header)?;
    }
    io.output("ampr_emu.index", 0, &images.index)?;
    io.output("ampr_assets.index", 0, &images.manifest)?;
    io.output("kraken.image", 0, format!("kraken/{basename}").as_bytes())?;
    Ok(())
}

fn package_basename(bytes: &[u8]) -> Result<&str, BuildError> {
    let name = str::from_utf8(bytes).map_err(|_| String::from("package basename must be UTF-8"))?;
    if name.is_empty() || name.len() > 255 || name.contains(['/', '\\', '\r', '\n', '\0']) {
        return Err(String::from("invalid package basename").into());
    }
    Ok(name)
}

#[no_mangle]
pub unsafe extern "C" fn sm_kraken_package_id(context: *mut c_void, size: u64,
                                              read: Read, output: *mut u8) -> bool {
    let reader = CallbackReader { context, read };
    let Ok(id) = lih::package_id(&reader, size) else { return false };
    unsafe { ptr::copy_nonoverlapping(id.as_ptr(), output, id.len()); }
    true
}

/// C retains ownership of the context and buffers for the duration of this call.
#[no_mangle]
pub unsafe extern "C" fn sm_kraken_build_index(
    context: *mut c_void, size: u64, mtime: i64, basename: *const u8, basename_len: usize,
    read: Read, write: Write, loose_size: LooseSize, error: *mut u8, error_len: usize,
) -> i32 {
    let io = PackageIo { context, read, write, loose_size };
    let result = package_basename(unsafe { slice::from_raw_parts(basename, basename_len) })
        .and_then(|name| prepare(&io, size, mtime, name));
    unsafe { report_result(result, error, error_len) }
}

/// Build a cache for a base PKG and one compatible debug LIH.
#[no_mangle]
pub unsafe extern "C" fn sm_kraken_build_patched_index(
    context: *mut c_void, size: u64, patch_size: u64, mtime: i64, basename: *const u8,
    basename_len: usize, read: Read, read_patch: Read, write: Write, loose_size: LooseSize,
    progress: Progress, is_cancelled: unsafe extern "C" fn(*mut c_void) -> i32,
    fast: bool, load_verified: LoadVerification, save_verified: SaveVerification,
    error: *mut u8, error_len: usize,
) -> i32 {
    let io = PackageIo { context, read, write, loose_size };
    let patch = CallbackReader { context, read: read_patch };
    let verification = lih::Verification {
        fast,
        load: Some(&|data| unsafe { load_verified(context, data.as_mut_ptr(), data.len()) }),
        save: Some(&|data| unsafe { save_verified(context, data.as_ptr(), data.len()) }),
    };
    let result = package_basename(unsafe { slice::from_raw_parts(basename, basename_len) })
        .and_then(|name| prepare_patched(&io, &patch, size, patch_size, mtime, name, &verification,
            |phase, done, total| unsafe { progress(context, phase, done, total) },
            || unsafe { is_cancelled(context) != 0 }));
    unsafe { report_result(result, error, error_len) }
}

unsafe fn report_result(result: Result<(), BuildError>, error: *mut u8, error_len: usize) -> i32 {
    match result {
        Ok(()) => 0,
        Err(error_kind) => {
            let (code, msg) = match error_kind {
                BuildError::Other(msg) => (-1, msg),
                BuildError::Decode(msg) => (-2, msg),
            };
            if error_len > 0 {
                let length = msg.len().min(error_len - 1);
                unsafe {
                    ptr::copy_nonoverlapping(msg.as_ptr(), error, length);
                    *error.add(length) = 0;
                }
            }
            code
        }
    }
}

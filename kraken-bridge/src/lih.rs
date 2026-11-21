//! Read an updated plaintext package from a debug LIH and its matching base.
//! Block placement follows Dump Forge's LIH reader. Blocks can move between
//! versions, so use the image digests even for the data supplied by the base.

use alloc::{collections::BTreeMap, format, string::String, vec, vec::Vec};
use core::{cell::RefCell, ops::Range};
use kraken_mount_pkg::{Error, PRead};
use sha3::{Digest, Sha3_256};

pub const PAGE: u64 = 0x10000;
const CNT_HEADER_SIZE: usize = 0x1000;
const ENTRY_SIZE: usize = 32;
const IMAGE_DIGESTS: u32 = 0x040a;
const MAX_IMAGE: u64 = 1 << 40;

fn le64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

fn be32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn corrupt(message: impl Into<String>) -> Error {
    Error::Corrupt(message.into())
}

fn unsupported(message: &str) -> Error {
    Error::Unsupported(message.into())
}

pub fn read_exact(io: &impl PRead, mut offset: u64, mut out: &mut [u8]) -> Result<(), Error> {
    while !out.is_empty() {
        let count = io.pread(offset, out)?;
        if count == 0 || count > out.len() {
            return Err(corrupt(format!("short read at {offset:#x}")));
        }
        offset += count as u64;
        out = &mut out[count..];
    }
    Ok(())
}

struct Image {
    pfs: Range<u64>,
    cnt: u64,
    superblock: u64,
}

impl Image {
    fn read(io: &impl PRead, start: u64) -> Result<Self, Error> {
        let mut header = [0; 0x68];
        read_exact(io, start, &mut header)?;
        if &header[..4] != b"\x7fFIH" || header[5] != 0 || header[6..8] != [3, 0] {
            return Err(unsupported("LIH requires debug FIH version 3 packages"));
        }
        let pfs = le64(&header, 0x10);
        let size = le64(&header, 0x18);
        let superblock = le64(&header, 0x20);
        let cnt = le64(&header, 0x58);
        let end = pfs.checked_add(size).ok_or_else(|| corrupt("outer image overflows"))?;
        if pfs < PAGE || pfs % PAGE != 0 || size == 0 || size % PAGE != 0 ||
            end > MAX_IMAGE || superblock < pfs || superblock >= end ||
            superblock % PAGE != 0 || cnt < PAGE || le64(&header, 0x60) != PAGE
        {
            return Err(corrupt("invalid debug FIH image layout"));
        }
        Ok(Self { pfs: pfs..end, cnt, superblock })
    }

    fn blocks(&self) -> usize {
        ((self.pfs.end - self.pfs.start) / PAGE) as usize
    }
}

type BlockDigest = [u8; 32];

fn digest(data: &[u8]) -> BlockDigest {
    let mut hash: BlockDigest = Sha3_256::digest(data).into();
    hash.reverse(); // imagedigs stores SHA3-256 in reverse byte order.
    hash
}

fn region(start: u64, size: u64, limit: u64) -> Result<Range<u64>, Error> {
    match start.checked_add(size) {
        Some(end) if end <= limit => Ok(start..end),
        _ => Err(corrupt("package region is outside the file")),
    }
}

struct Container {
    content_id: [u8; 0x24],
    digests: Vec<BlockDigest>,
}

fn container_metadata(io: &impl PRead, file: &Range<u64>) -> Result<(Vec<u8>, Vec<u8>), Error> {
    let size = file.end - file.start;
    region(0, CNT_HEADER_SIZE as u64, size)?;
    let mut header = vec![0; CNT_HEADER_SIZE];
    read_exact(io, file.start, &mut header)?;
    if &header[..4] != b"\x7fCNT" {
        return Err(corrupt("missing CNT container in package"));
    }
    let count = be32(&header, 0x10) as usize;
    if count > 4096 {
        return Err(corrupt("too many CNT entries"));
    }
    let table = region(u64::from(be32(&header, 0x18)), (count * ENTRY_SIZE) as u64, size)?;
    let mut rows = vec![0; count * ENTRY_SIZE];
    read_exact(io, file.start + table.start, &mut rows)?;
    Ok((header, rows))
}

fn image_digest_range(rows: &[u8], size: u64, blocks: usize) -> Result<Option<Range<u64>>, Error> {
    let Some(row) = rows.chunks_exact(ENTRY_SIZE).find(|row| be32(row, 0) == IMAGE_DIGESTS) else {
        return Ok(None);
    };
    let bytes = blocks as u64 * 32;
    if u64::from(be32(row, 20)) != bytes || be32(row, 8) & 0x8000_0000 != 0 {
        return Err(corrupt("invalid imagedigs in CNT container"));
    }
    region(u64::from(be32(row, 16)), bytes, size).map(Some)
}

fn container(io: &impl PRead, file: Range<u64>, blocks: usize) -> Result<Container, Error> {
    let (header, rows) = container_metadata(io, &file)?;
    let digests = image_digest_range(&rows, file.end - file.start, blocks)?
        .ok_or_else(|| corrupt("missing imagedigs in CNT container"))?;
    let at = file.start + digests.start;
    let mut digests = Vec::with_capacity(blocks);
    let mut batch = vec![0; PAGE as usize];
    while digests.len() < blocks {
        let start = digests.len() * 32;
        let size = (blocks * 32 - start).min(batch.len());
        read_exact(io, at + start as u64, &mut batch[..size])?;
        digests.extend(batch[..size].chunks_exact(32).map(|bytes| {
            let hash: BlockDigest = bytes.try_into().unwrap();
            hash
        }));
    }
    let content_id = header[0x40..0x64].try_into().unwrap();
    Ok(Container { content_id, digests })
}

/// Identify the same container after a copy, independent of its filesystem times.
/// Read the image digest table when present so different image layouts or data
/// advertised by the container produce different IDs without hashing the whole file.
pub fn package_id(io: &impl PRead, len: u64) -> Result<[u8; 32], Error> {
    if len < PAGE {
        return Err(corrupt("package is shorter than its FIH header page"));
    }
    let mut hash = Sha3_256::new();
    hash.update(b"SMP Kraken package ID v1");
    hash.update(len.to_le_bytes());
    let mut page = vec![0; PAGE as usize];
    read_exact(io, 0, &mut page)?;
    if &page[..4] != b"\x7fFIH" || page[5] != 0 {
        return Err(unsupported("expected a debug FIH base package"));
    }
    hash.update(&page);
    let cnt = le64(&page, 0x58);
    let image = Image::read(io, 0).ok();
    let cnt_end = image.as_ref().filter(|image| cnt < image.pfs.start)
        .map_or(len, |image| image.pfs.start.min(len));
    if cnt < PAGE || cnt >= cnt_end {
        return Err(corrupt("CNT lies outside the package"));
    }
    let (header, rows) = container_metadata(io, &(cnt..cnt_end))?;
    hash.update(&header);
    hash.update(&rows);
    if let Some(image) = image {
        if let Some(range) = image_digest_range(&rows, cnt_end - cnt, image.blocks())? {
            let mut at = range.start;
            while at < range.end {
                let count = (range.end - at).min(PAGE) as usize;
                read_exact(io, cnt + at, &mut page[..count])?;
                hash.update(&page[..count]);
                at += count as u64;
            }
        }
    }
    read_exact(io, len - PAGE, &mut page)?;
    hash.update(&page);
    Ok(hash.finalize().into())
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Base,
    Patch,
}

#[derive(Default)]
pub struct Verification<'a> {
    pub fast: bool,
    pub load: Option<&'a dyn Fn(&mut [u8]) -> bool>,
    pub save: Option<&'a dyn Fn(&[u8])>,
}

fn is_verified(bits: &[u8], page: usize) -> bool {
    bits[page / 8] & (1 << (page % 8)) != 0
}

fn mark_verified(bits: &mut [u8], page: usize) {
    bits[page / 8] |= 1 << (page % 8);
}

#[derive(Clone, Copy)]
pub struct Run {
    pub source: Source,
    pub offset: u64,
    pub len: u64,
}

pub struct View<'a, B, P> {
    base: &'a B,
    patch: &'a P,
    image: Image,
    header_at: u64,
    cnt: Range<u64>,
    // The top bit of a location's offset distinguishes patch from base.
    blocks: Vec<u64>,
    base_start: u64,
    base_digests: Vec<BlockDigest>,
    verified: RefCell<Vec<u8>>,
}

const PATCH: u64 = 1 << 63;

impl<'a, B: PRead, P: PRead> View<'a, B, P> {
    pub fn open(base: &'a B, base_len: u64, patch: &'a P, patch_len: u64,
                verification: &Verification<'_>, cancelled: impl Fn() -> bool,
                progress: impl Fn(u64, u64)) -> Result<Self, Error> {
        let mut head = [0; 0x40];
        read_exact(patch, 0, &mut head)?;
        if &head[..4] != b"\x7fLIH" || head[6..8] != [1, 0] {
            return Err(unsupported("expected a version 1 LIH patch"));
        }
        if le64(&head, 0x18) != patch_len {
            return Err(corrupt("LIH file size differs from its header"));
        }
        let header = region(le64(&head, 0x20), le64(&head, 0x28), patch_len)?;
        let cnt = region(le64(&head, 0x30), le64(&head, 0x38), patch_len)?;
        if header.start < PAGE || header.start % PAGE != 0 || header.end - header.start != PAGE ||
            cnt.start <= header.end || cnt.start % PAGE != 0 || patch_len > MAX_IMAGE
        {
            return Err(corrupt("invalid LIH regions"));
        }
        let image = Image::read(patch, header.start)?;
        let target_cnt = region(image.cnt, cnt.end - cnt.start, MAX_IMAGE)?;
        if target_cnt.start < image.pfs.end && image.pfs.start < target_cnt.end {
            return Err(corrupt("LIH target CNT overlaps its outer image"));
        }
        let original = Image::read(base, 0)?;
        if original.pfs.end > base_len || original.cnt >= base_len ||
            original.cnt >= original.pfs.start && original.cnt < original.pfs.end
        {
            return Err(corrupt("invalid base FIH regions"));
        }
        let base_cnt_end = if original.cnt < original.pfs.start { original.pfs.start } else { base_len };
        let original_cnt = container(base, original.cnt..base_cnt_end, original.blocks())?;
        let updated_cnt = container(patch, cnt.clone(), image.blocks())?;
        if original_cnt.content_id[0] == 0 || original_cnt.content_id != updated_cnt.content_id {
            return Err(unsupported("LIH content ID does not match the base package"));
        }
        let wanted = updated_cnt.digests;
        let mut stored = BTreeMap::new();
        let mut batch = vec![0; 16 * PAGE as usize];
        let mut at = header.end;
        while at < cnt.start {
            let len = (cnt.start - at).min(batch.len() as u64) as usize;
            read_exact(patch, at, &mut batch[..len])?;
            for bytes in batch[..len].chunks_exact(PAGE as usize) {
                stored.insert(digest(bytes), at);
                at += PAGE;
            }
            progress(at - header.end, cnt.start - header.end);
        }

        let mut base_by_digest = None;
        let mut used = alloc::collections::BTreeSet::new();
        let mut blocks = Vec::with_capacity(wanted.len());
        for (index, hash) in wanted.into_iter().enumerate() {
            if index % 4096 == 0 && cancelled() {
                return Err(corrupt("LIH mapping cancelled"));
            }
            let offset = if original_cnt.digests.get(index) == Some(&hash) {
                if let Some(&at) = stored.get(&hash) {
                    used.insert(at);
                }
                original.pfs.start + index as u64 * PAGE
            } else if let Some(&at) = stored.get(&hash) {
                used.insert(at);
                PATCH | at
            } else {
                let index = base_by_digest.get_or_insert_with(|| {
                    original_cnt.digests.iter().copied().enumerate().map(|(i, hash)| (hash, i as u32))
                        .collect::<BTreeMap<_, _>>()
                });
                let Some(&i) = index.get(&hash) else {
                    return Err(unsupported("LIH needs blocks absent from this base package"));
                };
                original.pfs.start + u64::from(i) * PAGE
            };
            blocks.push(offset);
        }
        if used.len() != stored.len() {
            return Err(corrupt("some LIH blocks do not match its image digests"));
        }
        let mut verified = vec![0; original.blocks().div_ceil(8)];
        if let Some(load) = verification.load {
            let mut saved = vec![0; 32 + verified.len()];
            if load(&mut saved) && saved[..32] == digest(&saved[32..]) {
                verified.copy_from_slice(&saved[32..]);
            }
        }
        let view = Self {
            base, patch, image, header_at: header.start, cnt, blocks,
            base_start: original.pfs.start, base_digests: original_cnt.digests,
            verified: RefCell::new(verified),
        };
        let mut marker = [0; 16];
        view.read_base(original.superblock + 0x370, &mut marker)?;
        if &marker != b"PPRPLAIN-NOAUTH!" {
            return Err(unsupported("LIH base image must be plaintext"));
        }
        read_exact(&view, view.image.superblock + 0x370, &mut marker)?;
        if &marker != b"PPRPLAIN-NOAUTH!" {
            return Err(unsupported("LIH target image must be plaintext"));
        }
        Ok(view)
    }

    pub fn save_verification(&self, verification: &Verification<'_>) {
        if let Some(save) = verification.save {
            let bits = self.verified.borrow();
            let mut record = Vec::with_capacity(32 + bits.len());
            record.extend_from_slice(&digest(&bits));
            record.extend_from_slice(&bits);
            save(&record);
        }
    }

    fn base_page(&self, offset: u64) -> usize {
        ((offset - self.base_start) / PAGE) as usize
    }

    fn verify_page(&self, page: usize, bytes: &[u8], verified: &mut [u8]) -> Result<(), Error> {
        if digest(bytes) != self.base_digests[page] {
            let offset = self.base_start + page as u64 * PAGE;
            return Err(corrupt(format!("base PKG block at {offset:#x} fails its LIH digest")));
        }
        mark_verified(verified, page);
        Ok(())
    }

    /// Validate referenced base bytes in physical order, once per block.
    /// Source files are fingerprinted again just before publication.
    pub fn verify_base(&self, progress: impl Fn(u64, u64)) -> Result<(), Error> {
        let mut verified = self.verified.borrow_mut();
        let mut needed = vec![0; verified.len()];
        for &offset in self.blocks.iter().filter(|&&offset| offset & PATCH == 0) {
            mark_verified(&mut needed, self.base_page(offset));
        }
        for (needed, verified) in needed.iter_mut().zip(verified.iter()) {
            *needed &= !verified;
        }
        let total = needed.iter().map(|byte| u64::from(byte.count_ones())).sum::<u64>() * PAGE;
        progress(0, total);
        if total == 0 {
            return Ok(());
        }
        let mut batch = vec![0; 16 * PAGE as usize];
        let mut done = 0;
        let mut page = 0;
        while page < self.base_digests.len() {
            if !is_verified(&needed, page) {
                page += 1;
                continue;
            }
            let end = (page + 1..(page + 16).min(self.base_digests.len()))
                .find(|&next| !is_verified(&needed, next))
                .unwrap_or((page + 16).min(self.base_digests.len()));
            let bytes = &mut batch[..(end - page) * PAGE as usize];
            read_exact(self.base, self.base_start + page as u64 * PAGE, bytes)?;
            for (block, data) in (page..end).zip(bytes.chunks_exact(PAGE as usize)) {
                self.verify_page(block, data, &mut verified)?;
            }
            done += bytes.len() as u64;
            progress(done, total);
            page = end;
        }
        Ok(())
    }

    // Fast mode validates whole pages before using any part in the built cache.
    // Ranges emitted directly as index offsets are left for the runtime to read.
    fn read_base(&self, mut offset: u64, mut out: &mut [u8]) -> Result<(), Error> {
        if out.is_empty() {
            return Ok(());
        }
        let mut verified = self.verified.borrow_mut();
        let first = self.base_page(offset);
        let last = self.base_page(offset + out.len() as u64 - 1);
        if (first..=last).all(|page| is_verified(&verified, page)) {
            return read_exact(self.base, offset, out);
        }
        let mut block = vec![0; PAGE as usize];
        while !out.is_empty() {
            let page = self.base_page(offset);
            let start = self.base_start + page as u64 * PAGE;
            let within = (offset - start) as usize;
            let count = out.len().min(block.len() - within);
            if is_verified(&verified, page) {
                read_exact(self.base, offset, &mut out[..count])?;
            } else {
                read_exact(self.base, start, &mut block)?;
                self.verify_page(page, &block, &mut verified)?;
                out[..count].copy_from_slice(&block[within..within + count]);
            }
            offset += count as u64;
            out = &mut out[count..];
        }
        Ok(())
    }

    pub fn len(&self) -> u64 {
        self.image.pfs.end.max(self.image.cnt + self.cnt.end - self.cnt.start)
    }

    pub fn locate(&self, at: u64) -> Result<Run, Error> {
        if at < PAGE {
            return Ok(Run { source: Source::Patch, offset: self.header_at + at, len: PAGE - at });
        }
        if self.image.pfs.contains(&at) {
            let relative = at - self.image.pfs.start;
            let index = (relative / PAGE) as usize;
            let within = relative % PAGE;
            let block = self.blocks[index];
            let source = if block & PATCH != 0 { Source::Patch } else { Source::Base };
            let mut end = index + 1;
            // Limit coalescing. Readers normally request at most one Kraken block.
            while end < self.blocks.len() && end - index < 16 &&
                self.blocks[end] == block + (end - index) as u64 * PAGE
            {
                end += 1;
            }
            return Ok(Run { source, offset: (block & !PATCH) + within, len: (end - index) as u64 * PAGE - within });
        }
        if at >= self.image.cnt && at - self.image.cnt < self.cnt.end - self.cnt.start {
            let relative = at - self.image.cnt;
            let offset = self.cnt.start + relative;
            return Ok(Run { source: Source::Patch, offset, len: self.cnt.end - offset });
        }
        Err(corrupt(format!("address {at:#x} falls outside the LIH image")))
    }
}

impl<B: PRead, P: PRead> PRead for View<'_, B, P> {
    fn pread(&self, mut at: u64, out: &mut [u8]) -> Result<usize, Error> {
        let mut done = 0;
        while done < out.len() && at < self.len() {
            let run = self.locate(at)?;
            let size = (out.len() - done).min(run.len as usize);
            let bytes = &mut out[done..done + size];
            match run.source {
                Source::Base => self.read_base(run.offset, bytes)?,
                Source::Patch => read_exact(self.patch, run.offset, bytes)?,
            }
            done += size;
            at += size as u64;
        }
        Ok(done)
    }
}

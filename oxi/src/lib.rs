#[repr(C)]
pub struct CompressedPNG {
    pub data: *mut u8,
    pub size: libc::size_t,
}

/// # Safety
///
/// Ensure “data” points to at least “size” bytes. The “data” member of
/// the returned struct must be freed with C’s “free” function.
#[no_mangle]
pub unsafe extern "C" fn optimize_png(data: *const u8, size: libc::size_t) -> CompressedPNG {
    let options = oxipng::Options::from_preset(6);

    let data_slice: &[u8] = std::slice::from_raw_parts(data, size);

    if let Ok(png) = oxipng::optimize_from_memory(data_slice, &options) {
        let compressed = CompressedPNG {
            data: libc::malloc(png.len()) as *mut u8,
            size: png.len(),
        };

        if !compressed.data.is_null() {
            std::ptr::copy_nonoverlapping(png.as_ptr(), compressed.data, png.len());
        }

        compressed
    } else {
        CompressedPNG {
            data: std::ptr::null_mut(),
            size: 0,
        }
    }
}

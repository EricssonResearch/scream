#[cfg(feature = "ecn-enabled")]
mod feature_specific {
    use crate::screamrx::imp::CAT;
    use gst_sys::GstMeta;
    use gtypes::GType;
    use libc::c_int;
    // Enums
    pub type GstNetEcnCp = c_int;

    #[derive(Copy, Clone)]
    #[repr(C)]
    pub struct GstNetEcnMeta {
        pub meta: GstMeta,
        pub cp: GstNetEcnCp,
    }

    #[link(name = "gstnet-1.0")]
    extern "C" {

        //=========================================================================
        // GstNetEcnMeta
        //=========================================================================
        pub fn gst_net_ecn_meta_api_get_type() -> GType;

    }

    pub fn get_ecn(buffer: gst::Buffer, pad: &gst::Pad) -> u8 {
        let ecn_ce: u8;
        let ecn_meta: *mut GstNetEcnMeta;
        unsafe {
            ecn_meta =
                gst_sys::gst_buffer_get_meta(buffer.as_mut_ptr(), gst_net_ecn_meta_api_get_type())
                    as *mut GstNetEcnMeta;
        }
        if ecn_meta.is_null() {
            gst::debug!(CAT, obj = pad, "Buffer did not contain an ECN meta");
            ecn_ce = 0;
        } else {
            unsafe {
                ecn_ce = (*ecn_meta).cp as u8;
            }
        }
        ecn_ce
    }
}
#[cfg(not(feature = "ecn-enabled"))]
mod default {
    pub fn get_ecn(_buffer: gst::Buffer, _pad: &gst::Pad) -> u8 {
        0
    }
}
#[cfg(feature = "ecn-enabled")]
pub use feature_specific::get_ecn;

#[cfg(not(feature = "ecn-enabled"))]
pub use default::get_ecn;

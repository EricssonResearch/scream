use crate::glib::translate::IntoGlibPtr;
use glib::prelude::*;
use glib::subclass::prelude::*;

use gst::prelude::*;
use gst::subclass::prelude::*;

use std::convert::TryInto;
use std::ffi::CString;
use std::os::raw::c_char;
use std::sync::Mutex;

pub use gstreamer_rtp::rtp_buffer::compare_seqnum;
pub use gstreamer_rtp::rtp_buffer::RTPBuffer;
pub use gstreamer_rtp::rtp_buffer::RTPBufferExt;

extern crate gstreamer_video as gstv;

use once_cell::sync::Lazy;

const DEFAULT_CURRENT_MAX_BITRATE: u32 = 0;

// Property value storage
#[derive(Debug, Clone)]
struct Settings {
    params: Option<String>,
    current_max_bitrate: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            params: None,
            current_max_bitrate: DEFAULT_CURRENT_MAX_BITRATE,
        }
    }
}

// static STREAMTX_PTR: Option<&Screamtx>  = None;

// Struct containing all the element data
#[repr(C)]
pub struct Screamtx {
    srcpad: gst::Pad,
    sinkpad: gst::Pad,
    rtcp_srcpad: gst::Pad,
    rtcp_sinkpad: gst::Pad,
    settings: Mutex<Settings>,
}

static CAT: Lazy<gst::DebugCategory> = Lazy::new(|| {
    gst::DebugCategory::new(
        "screamtx",
        gst::DebugColorFlags::empty(),
        Some("Screamtx Element"),
    )
});

impl Screamtx {
    // Called whenever a new buffer is passed to our sink pad. Here buffers should be processed and
    // whenever some output buffer is available have to push it out of the source pad.
    // Here we just pass through all buffers directly
    //
    // See the documentation of gst::Buffer and gst::BufferRef to see what can be done with
    // buffers.
    fn sink_chain(
        &self,
        pad: &gst::Pad,
        element: &super::Screamtx,
        buffer: gst::Buffer,
    ) -> Result<gst::FlowSuccess, gst::FlowError> {
        // trace!(CAT, obj: pad, "gstscream Handling buffer {:?}", buffer);
        let rtp_buffer = RTPBuffer::from_buffer_readable(&buffer).unwrap();
        let seq = rtp_buffer.seq();
        let payload_type = rtp_buffer.payload_type();
        let timestamp = rtp_buffer.timestamp();
        let ssrc = rtp_buffer.ssrc();
        let marker = rtp_buffer.is_marker() as u8;

        trace!(
            CAT,
            obj: pad,
            "gstscream Handling rtp buffer seq {} payload_type {} timestamp {} ",
            seq,
            payload_type,
            timestamp
        );
        drop(rtp_buffer);
        let mut rate: u32 = 0;
        let mut force_idr: u32 = 0;
        unsafe {
            let size = buffer.size().try_into().unwrap();
            ScreamSenderPush(
                buffer.into_glib_ptr(),
                size,
                seq,
                payload_type,
                timestamp,
                ssrc,
                marker,
            );
            ScreamSenderGetTargetRate(&mut rate, &mut force_idr);
        }
        if rate != 0 {
            let mut settings = self.settings.lock().unwrap();
            rate /= 1000;
            let are_equal = settings.current_max_bitrate == rate;
            if !are_equal {
                settings.current_max_bitrate = rate;
            }
            drop(settings);
            if !are_equal {
                element.notify("current-max-bitrate");
            }
        }
        if force_idr != 0 {
            let event = gstv::UpstreamForceKeyUnitEvent::builder()
                .all_headers(true)
                .build();
            let rc = self.sinkpad.push_event(event);
            println!("imp.rs: force_idr rc {} enabled 1 ", rc);
        }
        glib::bitflags::_core::result::Result::Ok(gst::FlowSuccess::Ok)
    }

    // Called whenever an event arrives on the sink pad. It has to be handled accordingly and in
    // most cases has to be either passed to Pad::event_default() on this pad for default handling,
    // or Pad::push_event() on all pads with the opposite direction for direct forwarding.
    // Here we just pass through all events directly to the source pad.
    //
    // See the documentation of gst::Event and gst::EventRef to see what can be done with
    // events, and especially the gst::EventView type for inspecting events.
    fn sink_event(&self, pad: &gst::Pad, _element: &super::Screamtx, event: gst::Event) -> bool {
        log!(
            CAT,
            obj: pad,
            "gstscream Handling event {:?} {:?}",
            event,
            event.type_()
        );
        // println!("gstscream Handling sink event {:?}", event);
        self.srcpad.push_event(event)
    }

    // Called whenever a query is sent to the sink pad. It has to be answered if the element can
    // handle it, potentially by forwarding the query first to the peer pads of the pads with the
    // opposite direction, or false has to be returned. Default handling can be achieved with
    // Pad::query_default() on this pad and forwarding with Pad::peer_query() on the pads with the
    // opposite direction.
    // Here we just forward all queries directly to the source pad's peers.
    //
    // See the documentation of gst::Query and gst::QueryRef to see what can be done with
    // queries, and especially the gst::QueryView type for inspecting and modifying queries.
    fn sink_query(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        query: &mut gst::QueryRef,
    ) -> bool {
        log!(CAT, obj: pad, "gstscream Handling query {:?}", query);
        self.srcpad.peer_query(query)
    }

    fn rtcp_sink_chain(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        buffer: gst::Buffer,
    ) -> Result<gst::FlowSuccess, gst::FlowError> {
        // trace!(CAT, obj: pad, "gstscream Handling buffer {:?}", buffer);
        let bmr = buffer.map_readable().unwrap();
        let bmrsl = bmr.as_slice();
        let bmrslprt = bmrsl.as_ptr();
        let buffer_size: u32 = buffer.size().try_into().unwrap();
        trace!(
            CAT,
            obj: pad,
            "gstscream Handling rtcp buffer size {} ",
            buffer_size
        );
        let res = unsafe { ScreamSenderRtcpPush(bmrslprt, buffer_size) };
        drop(bmr);
        if res == 0 {
            self.rtcp_srcpad.push(buffer).unwrap();
        }
        glib::bitflags::_core::result::Result::Ok(gst::FlowSuccess::Ok)
    }

    // Called whenever an event arrives on the rtcp_sink pad. It has to be handled accordingly and in
    // most cases has to be either passed to Pad::event_default() on this pad for default handling,
    // or Pad::push_event() on all pads with the opposite direction for direct forwarding.
    // Here we just pass through all events directly to the source pad.
    //
    // See the documentation of gst::Event and gst::EventRef to see what can be done with
    // events, and especially the gst::EventView type for inspecting events.
    fn rtcp_sink_event(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        event: gst::Event,
    ) -> bool {
        log!(
            CAT,
            obj: pad,
            "gstscream Handling rtcp_sink event {:?} {:?}",
            event,
            event.type_()
        );
        self.rtcp_srcpad.push_event(event)
    }

    // Called whenever a query is sent to the rtcp_sink pad. It has to be answered if the element can
    // handle it, potentially by forwarding the query first to the peer pads of the pads with the
    // opposite direction, or false has to be returned. Default handling can be achieved with
    // Pad::query_default() on this pad and forwarding with Pad::peer_query() on the pads with the
    // opposite direction.
    // Here we just forward all queries directly to the source pad's peers.
    //
    // See the documentation of gst::Query and gst::QueryRef to see what can be done with
    // queries, and especially the gst::QueryView type for inspecting and modifying queries.
    fn rtcp_sink_query(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        query: &mut gst::QueryRef,
    ) -> bool {
        log!(
            CAT,
            obj: pad,
            "gstscream Handling rtcp_sink query {:?}",
            query
        );
        self.rtcp_srcpad.peer_query(query)
    }

    // Called whenever an event arrives on the source pad. It has to be handled accordingly and in
    // most cases has to be either passed to Pad::event_default() on the same pad for default
    // handling, or Pad::push_event() on all pads with the opposite direction for direct
    // forwarding.
    // Here we just pass through all events directly to the sink pad.
    //
    // See the documentation of gst::Event and gst::EventRef to see what can be done with
    // events, and especially the gst::EventView type for inspecting events.
    fn src_event(&self, pad: &gst::Pad, _element: &super::Screamtx, event: gst::Event) -> bool {
        log!(
            CAT,
            obj: pad,
            "gstscream src Handling event {:?} {:?}",
            event,
            event.type_()
        );
        self.sinkpad.push_event(event)
    }

    fn rtcp_src_event(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        event: gst::Event,
    ) -> bool {
        log!(
            CAT,
            obj: pad,
            "gstscream rtcp src Handling event {:?} {:?}",
            event,
            event.type_()
        );
        true
        // self.rtcp_sinkpad.push_event(event)
    }

    // Called whenever a query is sent to the source pad. It has to be answered if the element can
    // handle it, potentially by forwarding the query first to the peer pads of the pads with the
    // opposite direction, or false has to be returned. Default handling can be achieved with
    // Pad::query_default() on this pad and forwarding with Pad::peer_query() on the pads with the
    // opposite direction.
    // Here we just forward all queries directly to the sink pad's peers.
    //
    // See the documentation of gst::Query and gst::QueryRef to see what can be done with
    // queries, and especially the gst::QueryView type for inspecting and modifying queries.
    fn src_query(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        query: &mut gst::QueryRef,
    ) -> bool {
        log!(CAT, obj: pad, "gstscream Handling src query {:?}", query);
        self.sinkpad.peer_query(query)
    }
    fn rtcp_src_query(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamtx,
        query: &mut gst::QueryRef,
    ) -> bool {
        log!(
            CAT,
            obj: pad,
            "gstscream Handling rtcp src query {:?}",
            query
        );
        self.rtcp_sinkpad.peer_query(query)
    }
}
#[allow(improper_ctypes_definitions)]
extern "C" fn callback(stx: *const Screamtx, buf: gst::Buffer, is_push: u8) {
    trace!(
        CAT,
        "gstscream Handling buffer from scream {:?} is_push  {}",
        buf,
        is_push
    );
    if is_push == 1 {
        unsafe {
            let fls = (*stx).srcpad.pad_flags();
            //            if fls.contains(gst::PadFlags::FLUSHING) || fls.contains(gst::PadFlags::EOS)
            if fls.contains(gst::PadFlags::EOS) {
                println!("screamtx EOS {:?}", fls);
                drop(buf);
            } else if fls.contains(gst::PadFlags::FLUSHING) {
                println!("screamtx FL {:?}", fls);
                drop(buf);
            } else {
                (*stx)
                    .srcpad
                    .push(buf)
                    .expect("Screamtx callback srcpad.push failed");
            }
        }
    } else {
        drop(buf);
    }
}
#[link(name = "scream")]
extern "C" {
    fn ScreamSenderPush(
        buf: *mut gstreamer_sys::GstBuffer,
        size: u32,
        seq: u16,
        payload_type: u8,
        timestamp: u32,
        ssrc: u32,
        marker: u8,
    );
    fn ScreamSenderRtcpPush(s: *const u8, size: u32) -> u8;
    fn ScreamSenderStats(s: *mut u8, length: *mut u32, clear: u8);
    fn ScreamSenderStatsHeader(s: *mut u8, length: *mut u32);
    #[allow(improper_ctypes)]
    fn ScreamSenderPluginInit(
        s: *const c_char,
        stx: *const Screamtx,
        cb: extern "C" fn(stx: *const Screamtx, buf: gst::Buffer, is_push: u8),
    );
    fn ScreamSenderGetTargetRate(rate_p: *mut u32, force_idr_p: *mut u32);

}
// This trait registers our type with the GObject object system and
// provides the entry points for creating a new instance and setting
// up the class data
#[glib::object_subclass]
impl ObjectSubclass for Screamtx {
    const NAME: &'static str = "RsScreamtx";
    type Type = super::Screamtx;
    type ParentType = gst::Element;

    // Called when a new instance is to be created. We need to return an instance
    // of our struct here and also get the class struct passed in case it's needed
    fn with_class(klass: &Self::Class) -> Self {
        // Create our two pads from the templates that were registered with
        // the class and set all the functions on them.
        //
        // Each function is wrapped in catch_panic_pad_function(), which will
        // - Catch panics from the pad functions and instead of aborting the process
        //   it will simply convert them into an error message and poison the element
        //   instance
        // - Extract our Screamtx struct from the object instance and pass it to us
        //
        // Details about what each function is good for is next to each function definition
        let templ = klass.pad_template("sink").unwrap();
        let sinkpad = gst::Pad::builder_with_template(&templ, Some("sink"))
            .chain_function(|pad, parent, buffer| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || Err(gst::FlowError::Error),
                    |screamtx, element| screamtx.sink_chain(pad, element, buffer),
                )
            })
            .event_function(|pad, parent, event| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.sink_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.sink_query(pad, element, query),
                )
            })
            .build();

        let templ = klass.pad_template("rtcp_sink").unwrap();
        let rtcp_sinkpad = gst::Pad::builder_with_template(&templ, Some("rtcp_sink"))
            .chain_function(|pad, parent, buffer| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || Err(gst::FlowError::Error),
                    |screamtx, element| screamtx.rtcp_sink_chain(pad, element, buffer),
                )
            })
            .event_function(|pad, parent, event| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.rtcp_sink_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.rtcp_sink_query(pad, element, query),
                )
            })
            .build();

        let templ = klass.pad_template("src").unwrap();
        let srcpad = gst::Pad::builder_with_template(&templ, Some("src"))
            .event_function(|pad, parent, event| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.src_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.src_query(pad, element, query),
                )
            })
            .build();

        let templ = klass.pad_template("rtcp_src").unwrap();
        let rtcp_srcpad = gst::Pad::builder_with_template(&templ, Some("rtcp_src"))
            .event_function(|pad, parent, event| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.rtcp_src_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamtx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtx, element| screamtx.rtcp_src_query(pad, element, query),
                )
            })
            .build();

        let settings = Mutex::new(Default::default());

        // Return an instance of our struct and also include our debug category here.
        // The debug category will be used later whenever we need to put something
        // into the debug logs
        Self {
            srcpad,
            sinkpad,
            rtcp_srcpad,
            rtcp_sinkpad,
            settings,
        }
    }
}

// Implementation of glib::Object virtual methods
impl ObjectImpl for Screamtx {
    // Called right after construction of a new instance
    fn constructed(&self, obj: &Self::Type) {
        // Call the parent class' ::constructed() implementation first
        self.parent_constructed(obj);

        // Here we actually add the pads we created in Screamtx::new() to the
        // element so that GStreamer is aware of their existence.
        obj.add_pad(&self.sinkpad).unwrap();
        obj.add_pad(&self.rtcp_sinkpad).unwrap();
        obj.add_pad(&self.srcpad).unwrap();
        obj.add_pad(&self.rtcp_srcpad).unwrap();
    }
    // Called whenever a value of a property is changed. It can be called
    // at any time from any thread.

    // Metadata for the properties
    fn properties() -> &'static [glib::ParamSpec] {
        static PROPERTIES: Lazy<Vec<glib::ParamSpec>> = Lazy::new(|| {
            vec![
                glib::ParamSpecString::new(
                    "params",
                    "Params",
                    "scream lib command line args",
                    None,
                    glib::ParamFlags::READWRITE,
                ),
                glib::ParamSpecString::new(
                    "stats",
                    "Stats",
                    "screamtx get_property lib stats in csv format",
                    None,
                    glib::ParamFlags::READWRITE,
                ),
                glib::ParamSpecString::new(
                    "stats-clear",
                    "StatsClear",
                    "screamtx get_property lib stats in csv format and clear counters",
                    None,
                    glib::ParamFlags::READWRITE,
                ),
                glib::ParamSpecString::new(
                    "stats-header",
                    "StatsHeader",
                    "screamtx get_property lib stats-header in csv format",
                    None,
                    glib::ParamFlags::READWRITE,
                ),
                glib::ParamSpecUInt::new(
                    "current-max-bitrate",
                    "Current-max-bitrate",
                    "Current max bitrate in kbit/sec, set by scream or by application",
                    0,
                    u32::MAX,
                    DEFAULT_CURRENT_MAX_BITRATE,
                    glib::ParamFlags::READWRITE,
                ),
            ]
        });
        PROPERTIES.as_ref()
    }
    fn set_property(
        &self,
        obj: &Self::Type,
        _id: usize,
        value: &glib::Value,
        pspec: &glib::ParamSpec,
    ) {
        match pspec.name() {
            "params" => {
                let mut settings = self.settings.lock().unwrap();
                // self.state.lock().unwrap().is_none()
                settings.params = match value.get::<String>() {
                    Ok(params) => Some(params),
                    _ => unreachable!("type checked upstream"),
                };
                info!(
                    CAT,
                    obj: obj,
                    "Changing params  to {}",
                    settings.params.as_ref().unwrap()
                );
                let s0 = settings.params.as_ref().unwrap().as_str();
                let s = CString::new(s0).expect("CString::new failed");
                //                self.srcpad.to_glib_none()
                // STREAMTX_PTR = Some(&self);
                unsafe {
                    ScreamSenderPluginInit(s.as_ptr(), self, callback);
                }
            }
            "current-max-bitrate" => {
                let mut settings = self.settings.lock().unwrap();
                let rate = value.get().expect("type checked upstream");
                info!(
                    CAT,
                    obj: obj,
                    "Changing current-max-bitrate from {} to {}",
                    settings.current_max_bitrate,
                    rate
                );
                settings.current_max_bitrate = rate;
            }
            _ => unimplemented!(),
        }
    }

    // Called whenever a value of a property is read. It can be called
    // at any time from any thread.
    fn property(&self, _obj: &Self::Type, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
        match pspec.name() {
            "params" => {
                let settings = self.settings.lock().unwrap();
                settings.params.to_value()
            }
            "stats" => {
                let res = unsafe {
                    let mut dst = Vec::with_capacity(500);
                    let mut dstlen: u32 = 0;
                    let pdst = dst.as_mut_ptr();

                    ScreamSenderStats(pdst, &mut dstlen, 0);
                    dst.set_len(dstlen.try_into().unwrap());
                    dst
                };
                let str1 = String::from_utf8(res).unwrap();
                str1.to_value()
            }
            "stats-clear" => {
                let res = unsafe {
                    let mut dst = Vec::with_capacity(500);
                    let mut dstlen: u32 = 0;
                    let pdst = dst.as_mut_ptr();

                    ScreamSenderStats(pdst, &mut dstlen, 1);
                    dst.set_len(dstlen.try_into().unwrap());
                    dst
                };
                let str1 = String::from_utf8(res).unwrap();
                str1.to_value()
            }
            "stats-header" => {
                let res = unsafe {
                    let mut dst = Vec::with_capacity(500);
                    let mut dstlen: u32 = 0;
                    let pdst = dst.as_mut_ptr();

                    ScreamSenderStatsHeader(pdst, &mut dstlen);
                    dst.set_len(dstlen.try_into().unwrap());
                    dst
                };
                let str1 = String::from_utf8(res).unwrap();
                str1.to_value()
            }
            "current-max-bitrate" => {
                let settings = self.settings.lock().unwrap();
                settings.current_max_bitrate.to_value()
            }
            _ => unimplemented!(),
        }
    }
}

// Implementation of gst::Element virtual methods
impl GstObjectImpl for Screamtx {}
impl ElementImpl for Screamtx {
    // Set the element specific metadata. This information is what
    // is visible from gst-inspect-1.0 and can also be programatically
    // retrieved from the gst::Registry after initial registration
    // without having to load the plugin in memory.
    fn metadata() -> Option<&'static gst::subclass::ElementMetadata> {
        static ELEMENT_METADATA: Lazy<gst::subclass::ElementMetadata> = Lazy::new(|| {
            gst::subclass::ElementMetadata::new(
                "Screamtx",
                "Generic",
                "pass RTP packets to screamtx",
                "Jacob Teplitsky <jacob.teplitsky@ericsson.com>",
            )
        });
        Some(&*ELEMENT_METADATA)
    }

    // Create and add pad templates for our sink and source pad. These
    // are later used for actually creating the pads and beforehand
    // already provide information to GStreamer about all possible
    // pads that could exist for this type.
    //
    // Actual instances can create pads based on those pad templates
    fn pad_templates() -> &'static [gst::PadTemplate] {
        static PAD_TEMPLATES: Lazy<Vec<gst::PadTemplate>> = Lazy::new(|| {
            // Our element can accept any possible caps on both pads
            let caps = gst::Caps::new_simple("application/x-rtp", &[]);
            let src_pad_template = gst::PadTemplate::new(
                "src",
                gst::PadDirection::Src,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            let caps = gst::Caps::new_simple("application/x-rtcp", &[]);
            let rtcp_src_pad_template = gst::PadTemplate::new(
                "rtcp_src",
                gst::PadDirection::Src,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            let caps = gst::Caps::new_simple("application/x-rtp", &[]);
            let sink_pad_template = gst::PadTemplate::new(
                "sink",
                gst::PadDirection::Sink,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            let caps = gst::Caps::new_simple("application/x-rtcp", &[]);
            let rtp_sink_pad_template = gst::PadTemplate::new(
                "rtcp_sink",
                gst::PadDirection::Sink,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();
            vec![
                src_pad_template,
                rtcp_src_pad_template,
                sink_pad_template,
                rtp_sink_pad_template,
            ]
        });

        PAD_TEMPLATES.as_ref()
    }

    // Called whenever the state of the element should be changed. This allows for
    // starting up the element, allocating/deallocating resources or shutting down
    // the element again.
    fn change_state(
        &self,
        element: &Self::Type,
        transition: gst::StateChange,
    ) -> Result<gst::StateChangeSuccess, gst::StateChangeError> {
        info!(CAT, obj: element, "Changing state {:?}", transition);

        // Call the parent class' implementation of ::change_state()
        self.parent_change_state(element, transition)
    }
}

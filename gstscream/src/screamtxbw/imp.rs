#![allow(clippy::uninlined_format_args)]
use glib::prelude::*;
use glib::subclass::prelude::*;
use gst::prelude::*;
use gst::subclass::prelude::*;

use std::ffi::CString;
use std::os::raw::c_char;
use std::sync::Mutex;

pub use gstreamer_rtp::rtp_buffer::compare_seqnum;
pub use gstreamer_rtp::rtp_buffer::RTPBuffer;
pub use gstreamer_rtp::rtp_buffer::RTPBufferExt;

extern crate gstreamer_video as gstv;

use once_cell::sync::Lazy;

const DEFAULT_BITRATE: u32 = 1000;

// Property value storage
#[derive(Debug, Clone)]
struct Settings {
    params: Option<String>,
    bitrate: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            params: None,
            bitrate: DEFAULT_BITRATE,
        }
    }
}

// static STREAMTX_PTR: Option<&Screamtxbw>  = None;

// Struct containing all the element data
#[repr(C)]
pub struct Screamtxbw {
    srcpad: gst::Pad,
    sinkpad: gst::Pad,
    settings: Mutex<Settings>,
}

static CAT: Lazy<gst::DebugCategory> = Lazy::new(|| {
    gst::DebugCategory::new(
        "screamtxbw",
        gst::DebugColorFlags::empty(),
        Some("Screamtxbw Element"),
    )
});

impl Screamtxbw {
    // Called whenever a new buffer is passed to our sink pad. Here buffers should be processed and
    // whenever some output buffer is available have to push it out of the source pad.
    // Here we just pass through all buffers directly
    //
    // See the documentation of gst::Buffer and gst::BufferRef to see what can be done with
    // buffers.
    fn sink_chain(
        &self,
        _pad: &gst::Pad,
        _element: &super::Screamtxbw,
        _buffer: gst::Buffer,
    ) -> Result<gst::FlowSuccess, gst::FlowError> {
        glib::bitflags::_core::result::Result::Ok(gst::FlowSuccess::Ok)
    }

    // Called whenever an event arrives on the sink pad. It has to be handled accordingly and in
    // most cases has to be either passed to Pad::event_default() on this pad for default handling,
    // or Pad::push_event() on all pads with the opposite direction for direct forwarding.
    // Here we just pass through all events directly to the source pad.
    //
    // See the documentation of gst::Event and gst::EventRef to see what can be done with
    // events, and especially the gst::EventView type for inspecting events.
    fn sink_event(&self, pad: &gst::Pad, _element: &super::Screamtxbw, event: gst::Event) -> bool {
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
        _element: &super::Screamtxbw,
        query: &mut gst::QueryRef,
    ) -> bool {
        log!(CAT, obj: pad, "gstscream Handling query {:?}", query);
        self.srcpad.peer_query(query)
    }

    // Called whenever an event arrives on the source pad. It has to be handled accordingly and in
    // most cases has to be either passed to Pad::event_default() on the same pad for default
    // handling, or Pad::push_event() on all pads with the opposite direction for direct
    // forwarding.
    // Here we just pass through all events directly to the sink pad.
    //
    // See the documentation of gst::Event and gst::EventRef to see what can be done with
    // events, and especially the gst::EventView type for inspecting events.
    fn src_event(&self, pad: &gst::Pad, _element: &super::Screamtxbw, event: gst::Event) -> bool {
        if event.structure().unwrap().name() == "GstForceKeyUnit" {
            unsafe {
                ScreamTxBwPluginSetForceKeyUnit();
            }

            info!(
                CAT,
                obj: pad,
                "gstscreamtxbw src Handling event {:?} {:?}",
                event,
                event.type_()
            );
        }
        log!(
            CAT,
            obj: pad,
            "gstscream src Handling event {:?} {:?}",
            event,
            event.type_()
        );
        self.sinkpad.push_event(event)
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
        _element: &super::Screamtxbw,
        query: &mut gst::QueryRef,
    ) -> bool {
        log!(CAT, obj: pad, "gstscream Handling src query {:?}", query);
        self.sinkpad.peer_query(query)
    }
}

extern "C" fn callback(
    stx: *const Screamtxbw,
    len: u32,
    seq_nr: u16,
    ts: u32,
    pt: u8,
    is_mark: u8,
    ssrc: u32,
) {
    unsafe {
        let fls = (*stx).srcpad.pad_flags();
        trace!(CAT, "creamtxbw Handling buffer from scream len={}, seq_nr {}, ts {}, pt {}, is_mark {}, ssrc {} {:?}",
                   len, seq_nr, ts, pt, is_mark, ssrc, fls);
        if fls.contains(gst::PadFlags::EOS) {
            println!("screamtxbw EOS {:?}", fls);
            return;
        } else if fls.contains(gst::PadFlags::FLUSHING) {
            println!("screamtxbw FL {:?}", fls);
            return;
        }
        let mut buffer = gst::Buffer::new_rtp_with_sizes(len, 4, 0).unwrap();
        {
            let buffer = buffer.get_mut().unwrap();
            let mut rtp_buffer = RTPBuffer::from_buffer_writable(buffer).unwrap();
            rtp_buffer.set_seq(seq_nr);
            rtp_buffer.set_payload_type(pt);
            rtp_buffer.set_timestamp(ts);
            rtp_buffer.set_ssrc(ssrc);
            rtp_buffer.set_marker(is_mark != 0);
            drop(rtp_buffer);
        }
        (*stx)
            .srcpad
            .push(buffer)
            .expect("Screamtxbw callback srcpad.push failed");
    }
}
#[link(name = "scream")]
extern "C" {
    #[allow(improper_ctypes)]
    fn ScreamTxBwPluginInit(
        s: *const c_char,
        stx: *const Screamtxbw,
        cb: extern "C" fn(
            stx: *const Screamtxbw,
            len: u32,
            seq_nr: u16,
            ts: u32,
            pt: u8,
            is_mark: u8,
            ssrc: u32,
        ),
    );
    fn ScreamTxBwPluginSetTargetRate(rate: u32);
    fn ScreamTxBwPluginSetForceKeyUnit();

}
// This trait registers our type with the GObject object system and
// provides the entry points for creating a new instance and setting
// up the class data
#[glib::object_subclass]
impl ObjectSubclass for Screamtxbw {
    const NAME: &'static str = "RsScreamtxbw";
    type Type = super::Screamtxbw;
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
        // - Extract our Screamtxbw struct from the object instance and pass it to us
        //
        // Details about what each function is good for is next to each function definition
        let templ = klass.pad_template("sink").unwrap();
        let sinkpad = gst::Pad::builder_from_template(&templ)
            .chain_function(|pad, parent, buffer| {
                Screamtxbw::catch_panic_pad_function(
                    parent,
                    || Err(gst::FlowError::Error),
                    |screamtxbw| screamtxbw.sink_chain(pad, &screamtxbw.obj(), buffer),
                )
            })
            .event_function(|pad, parent, event| {
                Screamtxbw::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtxbw| screamtxbw.sink_event(pad, &screamtxbw.obj(), event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamtxbw::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtxbw| screamtxbw.sink_query(pad, &screamtxbw.obj(), query),
                )
            })
            .build();

        let templ = klass.pad_template("src").unwrap();
        let srcpad = gst::Pad::builder_from_template(&templ)
            .event_function(|pad, parent, event| {
                Screamtxbw::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtxbw| screamtxbw.src_event(pad, &screamtxbw.obj(), event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamtxbw::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamtxbw| screamtxbw.src_query(pad, &screamtxbw.obj(), query),
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
            settings,
        }
    }
}

// Implementation of glib::Object virtual methods
impl ObjectImpl for Screamtxbw {
    // Called right after construction of a new instance
    fn constructed(&self) {
        // Call the parent class' ::constructed() implementation first
        self.parent_constructed();

        // Here we actually add the pads we created in Screamtxbw::new() to the
        // element so that GStreamer is aware of their existence.
        self.obj().add_pad(&self.sinkpad).unwrap();
        self.obj().add_pad(&self.srcpad).unwrap();
    }
    // Called whenever a value of a property is changed. It can be called
    // at any time from any thread.

    // Metadata for the properties
    fn properties() -> &'static [glib::ParamSpec] {
        static PROPERTIES: Lazy<Vec<glib::ParamSpec>> = Lazy::new(|| {
            vec![
                glib::ParamSpecString::builder("params")
                    .nick("Params")
                    .blurb("scream lib command line args")
                    .build(),
                glib::ParamSpecUInt::builder("bitrate")
                    .nick("bitrate")
                    .blurb("Bitrate in kbit/sec (0 = from NVENC preset)")
                    .minimum(0)
                    .maximum(u32::MAX)
                    .default_value(DEFAULT_BITRATE)
                    .build(),
            ]
        });
        PROPERTIES.as_ref()
    }
    fn set_property(&self, _id: usize, value: &glib::Value, pspec: &glib::ParamSpec) {
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
                    imp: self,
                    "Changing params  to {}",
                    settings.params.as_ref().unwrap()
                );
                let s0 = settings.params.as_ref().unwrap().as_str();
                let s = CString::new(s0).expect("CString::new failed");
                //                self.srcpad.to_glib_none()
                // STREAMTX_PTR = Some(&self);
                unsafe {
                    ScreamTxBwPluginInit(s.as_ptr(), self, callback);
                }
            }
            "bitrate" => {
                let mut settings = self.settings.lock().unwrap();
                let rate = value.get().expect("type checked upstream");
                info!(
                    CAT,
                    imp: self,
                    "Changing bitrate from {} to {}",
                    settings.bitrate,
                    rate
                );
                settings.bitrate = rate;
                unsafe {
                    ScreamTxBwPluginSetTargetRate(rate);
                }
            }
            _ => unimplemented!(),
        }
    }

    // Called whenever a value of a property is read. It can be called
    // at any time from any thread.
    fn property(&self, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
        match pspec.name() {
            "params" => {
                let settings = self.settings.lock().unwrap();
                settings.params.to_value()
            }
            "current-max-bitrate" => {
                let settings = self.settings.lock().unwrap();
                settings.bitrate.to_value()
            }
            _ => unimplemented!(),
        }
    }
}

// Implementation of gst::Element virtual methods
impl GstObjectImpl for Screamtxbw {}
impl ElementImpl for Screamtxbw {
    // Set the element specific metadata. This information is what
    // is visible from gst-inspect-1.0 and can also be programatically
    // retrieved from the gst::Registry after initial registration
    // without having to load the plugin in memory.
    fn metadata() -> Option<&'static gst::subclass::ElementMetadata> {
        static ELEMENT_METADATA: Lazy<gst::subclass::ElementMetadata> = Lazy::new(|| {
            gst::subclass::ElementMetadata::new(
                "Screamtxbw",
                "Generic",
                "generate RTP packets",
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
            let caps = gst::Caps::new_empty_simple("application/x-rtp");
            let src_pad_template = gst::PadTemplate::new(
                "src",
                gst::PadDirection::Src,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            let caps = gst::Caps::new_empty_simple("application/x-rtp");
            let sink_pad_template = gst::PadTemplate::new(
                "sink",
                gst::PadDirection::Sink,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            vec![src_pad_template, sink_pad_template]
        });

        PAD_TEMPLATES.as_ref()
    }

    // Called whenever the state of the element should be changed. This allows for
    // starting up the element, allocating/deallocating resources or shutting down
    // the element again.
    fn change_state(
        &self,
        transition: gst::StateChange,
    ) -> Result<gst::StateChangeSuccess, gst::StateChangeError> {
        info!(CAT, imp: self, "Changing state {:?}", transition);

        // Call the parent class' implementation of ::change_state()
        self.parent_change_state(transition)
    }
}

use glib::subclass::prelude::*;
use gst::prelude::*;
use gst::subclass::prelude::*;

use crate::gst::prelude::PadExtManual;

use std::convert::TryInto;
use std::sync::Arc;
use std::sync::Mutex;

pub use gstreamer_rtp::rtp_buffer::compare_seqnum;
pub use gstreamer_rtp::rtp_buffer::RTPBuffer;
pub use gstreamer_rtp::rtp_buffer::RTPBufferExt;

use once_cell::sync::Lazy;

use super::ScreamRx;

struct ClockWait {
    clock_id: Option<gst::ClockId>,
    _flushing: bool,
}

pub struct Screamrx {
    srcpad: gst::Pad,
    rtcp_srcpad: Option<Arc<Mutex<gst::Pad>>>,
    sinkpad: gst::Pad,
    lib_data: Mutex<ScreamRx::ScreamRx>,
    clock_wait: Mutex<ClockWait>,
}

pub static CAT: Lazy<gst::DebugCategory> = Lazy::new(|| {
    gst::DebugCategory::new(
        "screamrx",
        gst::DebugColorFlags::empty(),
        Some("Screamrx Element"),
    )
});

impl Screamrx {
    // Called whenever a new buffer is passed to our sink pad. Here buffers should be processed and
    // whenever some output buffer is available have to push it out of the source pad.
    // Here we just pass through all buffers directly
    //
    // See the documentation of gst::Buffer and gst::BufferRef to see what can be done with
    // buffers.
    fn sink_chain(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamrx,
        buffer: gst::Buffer,
    ) -> Result<gst::FlowSuccess, gst::FlowError> {
        gst_trace!(CAT, obj: pad, "gstscream Handling buffer {:?}", buffer);
        let rtp_buffer = RTPBuffer::from_buffer_readable(&buffer).unwrap();
        let seq = rtp_buffer.seq();
        let payload_type = rtp_buffer.payload_type();
        let timestamp = rtp_buffer.timestamp();
        let ssrc = rtp_buffer.ssrc();
        let marker = rtp_buffer.is_marker();

        gst_trace!(
            CAT,
            obj: pad,
            "gstscream Handling rtp buffer seq {} payload_type {} timestamp {} ",
            seq,
            payload_type,
            timestamp
        );
        drop(rtp_buffer);
        let size: u32 = buffer.size().try_into().unwrap();
        // TBD get ECN
        let ecn_ce: u8 = 0;
        {
            let mut screamrx = self.lib_data.lock().unwrap();
            screamrx.ScreamReceiver(size, seq, payload_type, timestamp, ssrc, marker, ecn_ce);
        }
        self.srcpad.push(buffer)
    }

    // Called whenever an event arrives on the sink pad. It has to be handled accordingly and in
    // most cases has to be either passed to Pad::event_default() on this pad for default handling,
    // or Pad::push_event() on all pads with the opposite direction for direct forwarding.
    // Here we just pass through all events directly to the source pad.
    //
    // See the documentation of gst::Event and gst::EventRef to see what can be done with
    // events, and especially the gst::EventView type for inspecting events.
    fn sink_event(&self, pad: &gst::Pad, _element: &super::Screamrx, event: gst::Event) -> bool {
        gst_log!(
            CAT,
            obj: pad,
            "gstscream Handling event {:?} {:?}",
            event,
            event.type_()
        );
        if let gst::EventView::StreamStart(ev) = event.view() {
            let stream_id = ev.stream_id();
            println!(
                "gstscream Handling sink StreamStarT event {:?} ;  {:?}; {}",
                event, ev, stream_id
            );
            self.srcpad
                .push_event(gst::event::StreamStart::new(stream_id));
            let rtcp_srcpad = self.rtcp_srcpad.as_ref().unwrap().lock().unwrap();
            let name = "rtcp_src";
            let full_stream_id = rtcp_srcpad.create_stream_id(_element, Some(name));
            // FIXME group id
            rtcp_srcpad.push_event(gst::event::StreamStart::new(&full_stream_id));
            // rtcp_srcpad.push_event(gst::event::Caps::new(caps));
            // FIXME proper segment handling
            let segment = gst::FormattedSegment::<gst::ClockTime>::default();
            rtcp_srcpad.push_event(gst::event::Segment::new(&segment));
        }
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
        _element: &super::Screamrx,
        query: &mut gst::QueryRef,
    ) -> bool {
        gst_log!(CAT, obj: pad, "gstscream Handling query {:?}", query);
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
    fn src_event(&self, pad: &gst::Pad, _element: &super::Screamrx, event: gst::Event) -> bool {
        gst_log!(
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
        _element: &super::Screamrx,
        event: gst::Event,
    ) -> bool {
        gst_log!(
            CAT,
            obj: pad,
            "gstscream rtcp_src Handling event {:?} {:?}",
            event,
            event.type_()
        );
        false
        // self.sinkpad.push_event(event)
    }
    fn rtcp_src_query(
        &self,
        pad: &gst::Pad,
        _element: &super::Screamrx,
        query: &mut gst::QueryRef,
    ) -> bool {
        gst_log!(
            CAT,
            obj: pad,
            "gstscream Handling rtcp src_query {:?}",
            query
        );
        false
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
        _element: &super::Screamrx,
        query: &mut gst::QueryRef,
    ) -> bool {
        gst_log!(CAT, obj: pad, "gstscream Handling query {:?}", query);
        self.sinkpad.peer_query(query)
    }
}

// This trait registers our type with the GObject object system and
// provides the entry points for creating a new instance and setting
// up the class data
#[glib::object_subclass]
impl ObjectSubclass for Screamrx {
    const NAME: &'static str = "RsScreamrx";
    type Type = super::Screamrx;
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
        // - Extract our Screamrx struct from the object instance and pass it to us
        //
        // Details about what each function is good for is next to each function definition
        let templ = klass.pad_template("sink").unwrap();
        let sinkpad = gst::Pad::builder_with_template(&templ, Some("sink"))
            .chain_function(|pad, parent, buffer| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || Err(gst::FlowError::Error),
                    |screamrx, element| screamrx.sink_chain(pad, element, buffer),
                )
            })
            .event_function(|pad, parent, event| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamrx, element| screamrx.sink_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamrx, element| screamrx.sink_query(pad, element, query),
                )
            })
            .build();

        let templ = klass.pad_template("src").unwrap();
        let srcpad = gst::Pad::builder_with_template(&templ, Some("src"))
            .event_function(|pad, parent, event| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamrx, element| screamrx.src_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamrx, element| screamrx.src_query(pad, element, query),
                )
            })
            .build();

        let name = "rtcp_src";
        let templ = klass.pad_template(name).unwrap();
        let rtcp_srcpad = gst::Pad::builder_with_template(&templ, Some(name))
            .event_function(|pad, parent, event| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamrx, element| screamrx.rtcp_src_event(pad, element, event),
                )
            })
            .query_function(|pad, parent, query| {
                Screamrx::catch_panic_pad_function(
                    parent,
                    || false,
                    |screamrx, element| screamrx.rtcp_src_query(pad, element, query),
                )
            })
            .build();
        rtcp_srcpad.set_active(true).unwrap();
        /*
        if let Some(event) = pad.sticky_event(gst::EventType::StreamStart, 0) {
            if let gst::EventView::StreamStart(ev) = event.view() {
                stream_type = ev.stream().map(|s| s.stream_type());
            }
        }

                assert!(sinkpad.send_event(gst::event::StreamStart::new("test")));
        let mut events = Vec::new();

                events.push(gst::event::StreamStart::new(&pull.stream_id));

        let full_stream_id = rtcp_srcpad.create_stream_id(element, Some(name));
        // FIXME group id
        rtcp_srcpad.push_event(gst::event::StreamStart::new(&full_stream_id));
        // rtcp_srcpad.push_event(gst::event::Caps::new(caps));

        // FIXME proper segment handling
        let segment = gst::FormattedSegment::<gst::ClockTime>::default();
        rtcp_srcpad.push_event(gst::event::Segment::new(&segment));
        */
        let rtcp_srcpad = Some(Arc::new(Mutex::new(rtcp_srcpad)));
        let lib_data = Mutex::new(Default::default());

        let clock_wait = Mutex::new(ClockWait {
            clock_id: None,
            _flushing: true,
        });

        // Return an instance of our struct and also include our debug category here.
        // The debug category will be used later whenever we need to put something
        // into the debug logs
        Self {
            srcpad,
            rtcp_srcpad,
            sinkpad,
            lib_data,
            clock_wait,
        }
    }
}
// Implementation of glib::Object virtual methods
impl ObjectImpl for Screamrx {
    // Called right after construction of a new instance
    fn constructed(&self, obj: &Self::Type) {
        // Call the parent class' ::constructed() implementation first
        self.parent_constructed(obj);

        // Here we actually add the pads we created in Screamrx::new() to the
        // element so that GStreamer is aware of their existence.
        obj.add_pad(&self.sinkpad).unwrap();
        let rtcp_srcpad = self.rtcp_srcpad.as_ref().unwrap().lock().unwrap();
        obj.add_pad(&*rtcp_srcpad).unwrap();
        obj.add_pad(&self.srcpad).unwrap();
    }
}

// Implementation of gst::Element virtual methods
impl GstObjectImpl for Screamrx {}
impl ElementImpl for Screamrx {
    // Set the element specific metadata. This information is what
    // is visible from gst-inspect-1.0 and can also be programatically
    // retrieved from the gst::Registry after initial registration
    // without having to load the plugin in memory.
    fn metadata() -> Option<&'static gst::subclass::ElementMetadata> {
        // Set the element specific metadata. This information is what
        // is visible from gst-inspect-1.0 and can also be programatically
        // retrieved from the gst::Registry after initial registration
        // without having to load the plugin in memory.
        static ELEMENT_METADATA: Lazy<gst::subclass::ElementMetadata> = Lazy::new(|| {
            gst::subclass::ElementMetadata::new(
                "Screamrx",
                "Generc",
                "pass RTP packets to screamrx",
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

            // Create and add pad templates for our sink and source pad. These
            // are later used for actually creating the pads and beforehand
            // already provide information to GStreamer about all possible
            // pads that could exist for this type.

            // Our element can accept any possible caps on both pads
            let caps = gst::Caps::new_simple("application/x-rtp", &[]);
            let src_pad_template = gst::PadTemplate::new(
                "src",
                gst::PadDirection::Src,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            let sink_pad_template = gst::PadTemplate::new(
                "sink",
                gst::PadDirection::Sink,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            let caps = gst::Caps::new_any();
            let rtcp_src_pad_template = gst::PadTemplate::new(
                "rtcp_src",
                gst::PadDirection::Src,
                gst::PadPresence::Always,
                &caps,
            )
            .unwrap();

            vec![src_pad_template, rtcp_src_pad_template, sink_pad_template]
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
        match transition {
            gst::StateChange::NullToReady => {
                {
                    let mut screamrx = self.lib_data.lock().unwrap();
                    gst_log!(CAT, "screamrx.ScreamReceiverPluginInit()");
                    screamrx.ScreamReceiverPluginInit(self.rtcp_srcpad.clone());
                }

                gst_debug!(CAT, obj: element, "Waiting for 1s before retrying");
                let clock = gst::SystemClock::obtain();
                let wait_time = clock.time().unwrap() + gst::ClockTime::SECOND;
                let mut clock_wait = self.clock_wait.lock().unwrap();
                let timeout = clock.new_periodic_id(wait_time, gst::ClockTime::from_useconds(500));
                clock_wait.clock_id = Some(timeout.clone());
                let element_weak = element.downgrade();
                timeout
                    .wait_async(move |_clock, _time, _id| {
                        let element = match element_weak.upgrade() {
                            None => return,
                            Some(element) => element,
                        };

                        let lib_data = Screamrx::from_instance(&element);
                        let mut screamrx = lib_data.lib_data.lock().unwrap();
                        screamrx.periodic_flush();
                    })
                    .expect("Failed to wait async");
            }
            gst::StateChange::ReadyToNull => {
                let mut clock_wait = self.clock_wait.lock().unwrap();
                if let Some(clock_id) = clock_wait.clock_id.take() {
                    clock_id.unschedule();
                }
            }
            _ => (),
        }

        gst_trace!(CAT, obj: element, "Changing state {:?}", transition);
        // Call the parent class' implementation of ::change_state()
        self.parent_change_state(element, transition)
    }
}

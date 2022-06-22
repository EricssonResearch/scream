extern crate failure;
use failure::Error;

use std::env;

extern crate gtypes;

use crate::gst::glib::Cast;
use glib::ObjectExt;

extern crate gstreamer_video as gstv;
use crate::gst::prelude::ElementExt;
use crate::gstv::prelude::GstBinExt;
use crate::gstv::prelude::GstObjectExt;

extern crate chrono;

// #[macro_use]
extern crate gstreamer as gst;

fn main() {
    gst::init().expect("Failed to initialize");

    let main_loop = glib::MainLoop::new(None, false);

    start(&main_loop).expect("Failed to start");
}

pub fn start(main_loop: &glib::MainLoop) -> Result<(), Error> {
    let pls: String;

    pls = env::var("RECVPIPELINE").unwrap();
    println!("RECVPIPELINE={}", pls);
    let pipeline = gst::parse_launch(&pls).unwrap();

    let pipeline = pipeline.downcast::<gst::Pipeline>().unwrap();
    let pipeline_clone = pipeline.clone();
    let bin = pipeline.upcast::<gst::Bin>();
    let rtpbin = bin.by_name("rtpbin").unwrap();
    rtpbin.connect("new-jitterbuffer", false, move |_values| None);

    let pipeline = bin.upcast::<gst::Element>();
    pipeline
        .set_state(gst::State::Playing)
        .expect("Failed to set pipeline to `Playing`");

    let main_loop_clone = main_loop.clone();
    let bus = pipeline_clone.bus().unwrap();
    bus.add_watch(move |_, msg| {
        use gst::MessageView;

        // println!("bus {:?}", msg.view());
        let main_loop = &main_loop_clone;
        match msg.view() {
            MessageView::Eos(..) => {
                println!("### got Eos message");
                main_loop.quit();
            }
            MessageView::Error(err) => {
                println!(
                    "### error from {:?}: {} ({:?})",
                    err.src().map(|s| s.path_string()),
                    err.error(),
                    err.debug()
                );
                main_loop.quit();
            }
            _ => (),
        }

        glib::Continue(true)
    })
    .expect("failed to add bus watch");

    main_loop.run();
    pipeline
        .set_state(gst::State::Null)
        .expect("Failed to set pipeline to `Null`");

    Ok(())
}

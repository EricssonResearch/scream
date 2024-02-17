#![allow(clippy::uninlined_format_args)]
extern crate failure;
use failure::Error;

use std::env;

extern crate gtypes;

use gst::glib;
use gst::glib::prelude::*;
use gst::prelude::*;

extern crate chrono;

fn main() {
    gst::init().expect("Failed to initialize");

    let main_loop = glib::MainLoop::new(None, false);

    start(&main_loop).expect("Failed to start");
}

pub fn start(main_loop: &glib::MainLoop) -> Result<(), Error> {
    let pls: String = env::var("RECVPIPELINE").unwrap();
    println!("RECVPIPELINE={}", pls);
    let pipeline = gst::parse::launch(&pls).unwrap();

    let pipeline = pipeline.downcast::<gst::Pipeline>().unwrap();
    let pipeline_clone = pipeline.clone();
    let bin = pipeline.upcast::<gst::Bin>();
    let rtpbin = bin.by_name("r").unwrap();
    rtpbin.connect("new-jitterbuffer", false, move |_values| None);

    let pipeline = bin.upcast::<gst::Element>();
    pipeline
        .set_state(gst::State::Playing)
        .expect("Failed to set pipeline to `Playing`");

    let main_loop_clone = main_loop.clone();
    let bus = pipeline_clone.bus().unwrap();
    let _bus_watch = bus
        .add_watch(move |_, msg| {
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
            };
            glib::ControlFlow::Continue
        })
        .expect("failed to add bus watch");

    main_loop.run();
    pipeline
        .set_state(gst::State::Null)
        .expect("Failed to set pipeline to `Null`");

    Ok(())
}

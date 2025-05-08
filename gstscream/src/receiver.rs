#![allow(clippy::uninlined_format_args)]
extern crate failure;
use std::fmt::Display;

use clap::Parser;
use failure::Error;

extern crate gtypes;

use gst::glib;
use gst::glib::prelude::*;
use gst::prelude::*;

extern crate chrono;

#[derive(clap::Parser)]
#[command(version, about, long_about = None)]
struct Arguments {
    #[arg(env)]
    /// The gstreamer pipeline to use for the receiver application.
    recvpipeline: String,
    /// Encoder Id , such as 264, 265
    #[arg(env, default_value_t = 264)]
    enc_id: u32,
}

fn main() {
    let args = Arguments::parse();
    println!("{args}");
    gst::init().expect("Failed to initialize");

    let main_loop = glib::MainLoop::new(None, false);

    start(&main_loop, args).expect("Failed to start");
}

fn start(main_loop: &glib::MainLoop, args: Arguments) -> Result<(), Error> {
    let pls: String = args.recvpipeline;
    println!("RECVPIPELINE={}", pls);
    let pipeline = gst::parse::launch(&pls).unwrap();

    let pipeline = pipeline.downcast::<gst::Pipeline>().unwrap();
    let pipeline_clone = pipeline.clone();
    let bin = pipeline.upcast::<gst::Bin>();
    let rtpbin = bin.by_name("r").unwrap();
    rtpbin.connect("new-jitterbuffer", false, move |_values| None);
    rtpbin.connect("request-pt-map", false, move |values| {
        let encoder_name = if args.enc_id == 265 { "H265" } else { "H264" };
        let pt = values[2].get::<u32>().expect("Invalid argument");
        println!("got pt: {pt}");
        match pt {
            96 => Some(
                gst::Caps::builder("application/x-rtp")
                    .field("media", "video")
                    .field("clock-rate", 90000i32)
                    .field("encoding-name", encoder_name)
                    .field("rtcp-fb-nack-pli", true)
                    .build()
                    .to_value(),
            ),
            _ => None,
        }
    });

    let pipeline = bin.upcast::<gst::Element>();
    pipeline
        .set_state(gst::State::Playing)
        .expect("Failed to set pipeline to `Playing`");

    let main_loop_clone = main_loop.clone();
    let bus = pipeline_clone.bus().unwrap();
    let _bus_watch = bus
        .add_watch(move |_, msg| {
            use gst::MessageView;

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

impl Display for Arguments {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Configuration:\n\t- Pipeline {}", self.recvpipeline)
    }
}

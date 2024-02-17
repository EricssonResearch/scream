#![allow(clippy::uninlined_format_args)]
use failure::Error;
use std::env;
use std::process::exit;

extern crate argparse;
extern crate failure;
#[macro_use]
extern crate lazy_static;

use gst::glib;
use gst::prelude::*;

use argparse::{ArgumentParser, StoreOption, StoreTrue};

mod sender_util;

fn main() {
    gst::init().expect("Failed to initialize gst_init");

    let main_loop = glib::MainLoop::new(None, false);
    start(&main_loop).expect("Failed to start");
}

pub fn start(main_loop: &glib::MainLoop) -> Result<(), Error> {
    let mut ratemultiply_opt: Option<i32> = None;
    let mut verbose = false;
    {
        // this block limits scope of borrows by ap.refer() method
        let mut ap = ArgumentParser::new();
        ap.set_description("Sender");
        ap.refer(&mut verbose)
            .add_option(&["-v", "--verbose"], StoreTrue, "Be verbose");
        ap.refer(&mut ratemultiply_opt).add_option(
            &["-r", "--ratemultiply"],
            StoreOption,
            "Set ratemultiply",
        );

        ap.parse_args_or_exit();
    }

    let pls = env::var("SENDPIPELINE").unwrap();
    println!("Pipeline: {}", pls);
    let n_encoder0 = pls.matches("name=encoder0").count();
    let n_encoders = pls.matches("name=encoder").count();
    if n_encoder0 == 0 {
        println!("missing name=encoder0");
        exit(0);
    }

    let pipeline = gst::parse::launch(&pls).unwrap();
    let pipeline = pipeline.downcast::<gst::Pipeline>().unwrap();

    pipeline
        .set_state(gst::State::Playing)
        .expect("Failed to set pipeline to `Playing`");

    let pipeline = pipeline.downcast::<gst::Pipeline>().unwrap();

    let pipeline_clone = pipeline;
    /*  TBD
     * set ecn bits
     */
    for n in 0..n_encoders {
        let n_string = n.to_string();
        sender_util::stats(
            &pipeline_clone,
            n,
            &Some("screamtx".to_string() + &n_string),
        );
        sender_util::run_time_bitrate_set(
            &pipeline_clone,
            verbose,
            &Some("screamtx".to_string() + &n_string),
            &Some("encoder".to_string() + &n_string),
            ratemultiply_opt,
        );
    }

    let main_loop_cloned = main_loop.clone();
    let bus = pipeline_clone.bus().unwrap();
    let _bus_watch = bus
        .add_watch(move |_, msg| {
            use gst::MessageView;
            // println!("sender: {:?}", msg.view());
            match msg.view() {
                MessageView::Eos(..) => {
                    println!("Bus watch  Got eos");
                    main_loop_cloned.quit();
                }
                MessageView::Error(err) => {
                    println!(
                        "Error from {:?}: {} ({:?})",
                        err.src().map(|s| s.path_string()),
                        err.error(),
                        err.debug()
                    );
                }
                _ => (),
            };
            glib::ControlFlow::Continue
        })
        .expect("failed to add bus watch");

    main_loop.run();
    pipeline_clone
        .set_state(gst::State::Null)
        .expect("Failed to set pipeline to `Null`");
    println!("Done");
    Ok(())
}

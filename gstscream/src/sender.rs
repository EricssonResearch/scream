#![allow(clippy::uninlined_format_args)]
use clap::Parser;
use failure::Error;
use std::fmt::Display;
use std::process::exit;

extern crate failure;
#[macro_use]
extern crate lazy_static;

use gst::glib;
use gst::prelude::*;

mod sender_util;

#[derive(clap::Parser)]
#[command(version, about, long_about = None)]
struct Arguments {
    #[arg(short, long, default_value_t = false)]
    /// Produces verbose logs.
    verbose: bool,

    #[arg(short, long)]
    /// Rate multiplication factor.
    ratemultiply: Option<i32>,

    #[arg(env)]
    /// The gstreamer pipeline to use for the sender application.
    sendpipeline: String,

    #[arg(env, default_value_t = 1000)]
    /// The logging interval for sender statistics.
    sender_stats_timer: u32,

    #[arg(env, default_value = "sender_scream_stats.csv")]
    /// The logging interval for sender statistics.
    sender_stats_file_name: std::path::PathBuf,
}

fn main() {
    let args = Arguments::parse();
    println!("{}", args);

    gst::init().expect("Failed to initialize gst_init");

    let main_loop = glib::MainLoop::new(None, false);
    start(&main_loop, args).expect("Failed to start");
}

fn start(main_loop: &glib::MainLoop, args: Arguments) -> Result<(), Error> {
    let pls = args.sendpipeline;

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
            args.sender_stats_timer,
            args.sender_stats_file_name.clone(),
        );
        sender_util::run_time_bitrate_set(
            &pipeline_clone,
            args.verbose,
            &Some("screamtx".to_string() + &n_string),
            &Some("encoder".to_string() + &n_string),
            args.ratemultiply,
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
impl Display for Arguments {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Configuration:\n\t- Verbose: {},\n\t- Ratemultiply: {:?},\n\t- Pipeline: {},\n\t- Logging interval: {},\n\t- sender_stats_file: {}",
            self.verbose,
            self.ratemultiply,
            self.sendpipeline,
            self.sender_stats_timer,
            self.sender_stats_file_name.display()
        )
    }
}

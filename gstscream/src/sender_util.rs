#![allow(clippy::uninlined_format_args)]
use std::convert::TryInto;
use std::fs::File;
use std::io::Write;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use gst::glib;
use gst::prelude::*;

use glib::timeout_add;

#[derive(Default)]
struct RateInfo {
    rate: u32,
    st: Duration,
    count: u32,
}

pub fn stats(
    bin: &gst::Pipeline,
    n: usize,
    screamtx_name_opt: &Option<String>,
    sender_stats_timer: u32,
    mut sender_stats_file_name: std::path::PathBuf,
) {
    if sender_stats_timer == 0 {
        return;
    }

    if screamtx_name_opt.is_none() {
        println!("no scream name");
        return;
    }

    println!("SENDER_STATS_TIMER={}", sender_stats_timer);

    let pipeline_clock = bin.pipeline_clock();

    let repl_string = "_".to_owned() + &n.to_string() + ".csv";
    let file_name = match sender_stats_file_name.file_name() {
        Some(file) => file,
        None => {
            println!("Invalid SENDER_STATS_FILE_NAME.");
            return;
        }
    };
    let file_name = match file_name.to_str() {
        Some(file_name) => file_name,
        None => {
            println!("SENDER_STATS_FILE_NAME must be a utf8 encoded string");
            return;
        }
    }
    .to_string();

    sender_stats_file_name.set_file_name(file_name.replace(".csv", &repl_string));
    println!(
        "SENDER_STATS_FILE_NAME={}",
        sender_stats_file_name.display()
    );
    let mut out: File = File::create(&sender_stats_file_name).unwrap();

    let scream_name = screamtx_name_opt.as_ref().unwrap();
    let screamtx_e = match bin.by_name(scream_name) {
        Some(v) => v,
        None => {
            println!(" no {}", scream_name);
            return;
        }
    };

    let screamtx_e_clone = screamtx_e.clone();
    let stats_str_header = screamtx_e.property::<String>("stats-header");

    writeln!(out, "time-ns,{}", stats_str_header).unwrap();

    let outp_opt: Option<Arc<Mutex<File>>> = Some(Arc::new(Mutex::new(out)));

    timeout_add(
        Duration::from_millis(sender_stats_timer as u64),
        move || {
            let stats_str = screamtx_e_clone.property::<String>("stats");

            let tm = pipeline_clock.time();
            let ns = tm.unwrap().nseconds();
            let out_p = outp_opt.as_ref().unwrap();
            let mut fd = out_p.lock().unwrap();

            writeln!(fd, "{},{}", ns, stats_str).unwrap();
            glib::ControlFlow::Continue
        },
    );
}

lazy_static! {
    static ref RATE_INFO_PREV: Mutex<RateInfo> = Mutex::new(RateInfo {
        ..RateInfo::default()
    });
}

pub fn run_time_bitrate_set(
    bin: &gst::Pipeline,
    verbose: bool,
    screamtx_name_opt: &Option<String>,
    encoder_name_opt: &Option<String>,
    ratemultiply_opt: Option<i32>,
) {
    if encoder_name_opt.is_none() {
        println!("no encoder_name_opt");
        return;
    }
    let ratemultiply: u32 = ratemultiply_opt.unwrap_or(1).try_into().unwrap();
    println!(
        "{:?} {:?} {:?}",
        encoder_name_opt, screamtx_name_opt, ratemultiply_opt
    );
    let encoder_name = encoder_name_opt.as_ref().unwrap();
    println!("encoder_name {:?}", encoder_name);
    let encoder_name_clone = encoder_name.clone();
    let video = bin
        .by_name(encoder_name)
        .expect("Failed to by_name encoder");

    let video_cloned = video;
    match screamtx_name_opt.as_ref() {
        Some(scream_name) => {
            match bin.by_name(scream_name) {
                Some(scream) => {
                    let scream_cloned = scream.clone();
                    scream.connect("notify::current-max-bitrate", false,  move |_values| {
                        let rate = scream_cloned.property::<u32>("current-max-bitrate");
                        let rate = rate * ratemultiply;
                        let prev_br:u32 = video_cloned.property::<u32>("bitrate");
                        video_cloned
                            .set_property("bitrate", rate);
                        let n = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();

                        let mut rate_info_prev = RATE_INFO_PREV.lock().unwrap();
                        let rate_prev = rate_info_prev.rate;
                        let st_prev = rate_info_prev.st;
                        let diff = n.as_secs() - st_prev.as_secs();
                        if diff >= 1 {
                            if rate != rate_prev {
                                if verbose {
                                    println!("notif: {} {}.{:06} rate {:08} rate_prev {:08} time_prev {}.{:06} diff {} count {} prev_br {}",
                                             encoder_name_clone, n.as_secs(), n.subsec_micros(), rate, rate_prev, st_prev.as_secs(),
                                             st_prev.subsec_micros(), diff, rate_info_prev.count, prev_br);
                                }
                                rate_info_prev.rate = rate;
                                rate_info_prev.st = n;
                                rate_info_prev.count = 0;
                            }
                        } else {
                                rate_info_prev.count += 1;
                                // println!("count {}", rate_info_prev.count);
                        }
                        None
                    });
                }
                None => println!("no scream signal"),
            }
        }
        None => println!("no scream name"),
    }
}

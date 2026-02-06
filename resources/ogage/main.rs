extern crate evdev_rs as evdev;
extern crate mio;

use evdev::*;
use evdev::enums::*;
use std::io;
use std::fs::File;
use std::path::Path;
use std::process::Command;
use std::os::unix::io::AsRawFd;
use mio::{Poll,Events,Token,Interest};
use mio::unix::SourceFd;
use std::sync::{
    atomic::{AtomicBool, AtomicU8, Ordering},
    Arc,
};
use std::thread;
use std::time::Duration;

#[derive(Clone, Copy, PartialEq)]
enum RepeatAction {
    None = 0,
    BrightUp,
    BrightDown,
    VolUp,
    VolDown,
}

static HOTKEY:         EventCode = EventCode::EV_KEY(EV_KEY::BTN_TRIGGER_HAPPY4);
static BRIGHT_UP:      EventCode = EventCode::EV_KEY(EV_KEY::BTN_DPAD_UP);
static BRIGHT_DOWN:    EventCode = EventCode::EV_KEY(EV_KEY::BTN_DPAD_DOWN);
static GAMMA_UP:       EventCode = EventCode::EV_KEY(EV_KEY::BTN_DPAD_RIGHT);
static GAMMA_DOWN:     EventCode = EventCode::EV_KEY(EV_KEY::BTN_DPAD_LEFT);
static VOL_UP:         EventCode = EventCode::EV_KEY(EV_KEY::BTN_TR);
static VOL_DOWN:       EventCode = EventCode::EV_KEY(EV_KEY::BTN_TL);
static BRIGHT_DOWN2:   EventCode = EventCode::EV_KEY(EV_KEY::BTN_TL2);
static BRIGHT_UP2:     EventCode = EventCode::EV_KEY(EV_KEY::BTN_TR2);
static VOLUME_UP:      EventCode = EventCode::EV_KEY(EV_KEY::KEY_VOLUMEUP);
static VOLUME_DOWN:    EventCode = EventCode::EV_KEY(EV_KEY::KEY_VOLUMEDOWN);
static MUTE:           EventCode = EventCode::EV_KEY(EV_KEY::BTN_TRIGGER_HAPPY3);

/*fn blink1() {
    Command::new("brightnessctl").arg("-O").output().expect("Failed to execute brightnessctl");

    Command::new("brightnessctl").args(&["-T","1.5"]).output().expect("Failed to execute brightnessctl");
    Command::new("sleep").arg("0.1").output().expect("Failed to execute brightnessctl");

    Command::new("brightnessctl").arg("-I").output().expect("Failed to execute brightnessctl");
}

fn blink2() {
    Command::new("brightnessctl").arg("-O").output().expect("Failed to execute brightnessctl");

    Command::new("brightnessctl").args(&["-T","1.5"]).output().expect("Failed to execute brightnessctl");
    Command::new("sleep").arg("0.1").output().expect("Failed to execute brightnessctl");

    Command::new("brightnessctl").arg("-I").output().expect("Failed to execute brightnessctl");
    Command::new("sleep").arg("0.1").output().expect("Failed to execute brightnessctl");

    Command::new("brightnessctl").args(&["-T","1.5"]).output().expect("Failed to execute brightnessctl");
    Command::new("sleep").arg("0.1").output().expect("Failed to execute brightnessctl");

    Command::new("brightnessctl").arg("-I").output().expect("Failed to execute brightnessctl");
}*/

fn process_event(_dev: &Device, ev: &InputEvent, hotkey: bool, repeat_action: &Arc<AtomicU8>, repeat_active: &Arc<AtomicBool>) {
//    println!("Event: time {}.{} type {} code {} value {} hotkey {}",
//             ev.time.tv_sec,
//             ev.time.tv_usec,
//             ev.event_type,
//             ev.event_code,
//             ev.value,
//             hotkey);

    if hotkey && ev.value == 1 {
        if ev.event_code == BRIGHT_UP || ev.event_code == BRIGHT_UP2 {
            repeat_action.store(RepeatAction::BrightUp as u8, Ordering::Relaxed);
            repeat_active.store(true, Ordering::Relaxed);
        } else if ev.event_code == BRIGHT_DOWN || ev.event_code == BRIGHT_DOWN2 {
            repeat_action.store(RepeatAction::BrightDown as u8, Ordering::Relaxed);
            repeat_active.store(true, Ordering::Relaxed);
        } else if ev.event_code == VOL_UP {
            repeat_action.store(RepeatAction::VolUp as u8, Ordering::Relaxed);
            repeat_active.store(true, Ordering::Relaxed);
        } else if ev.event_code == VOL_DOWN {
            repeat_action.store(RepeatAction::VolDown as u8, Ordering::Relaxed);
            repeat_active.store(true, Ordering::Relaxed);
        }
        /*else if ev.event_code == PERF_MAX {
            Command::new("sudo").args(&["perfmax", "On"]).output().expect("Failed to execute performance");
            //blink1();
        }
        else if ev.event_code == PERF_NORM {
            Command::new("sudo").arg("perfnorm").output().expect("Failed to execute performance");
            //blink1();
        }*/
        else if ev.event_code == EventCode::EV_KEY(EV_KEY::KEY_POWER) && ev.value > 0 {
            //blink2();
            Command::new("finish.sh").spawn().ok().expect("Failed to execute shutdown process");
        }
        /*else if ev.event_code == DARK_ON {
            //Command::new("sudo").args(&["rfkill", "block", "all"]).output().expect("Failed to execute rfkill");
            //blink1();
        //}
        //else if ev.event_code == DARK_OFF {
            //Command::new("sudo").args(&["rfkill", "unblock", "all"]).output().expect("Failed to execute rfkill");
            //blink1();
        }*/
        else if ev.event_code == MUTE && ev.value > 0 {
            Command::new("mute_toggle.sh").output().expect("Failed to execute amixer");
        }
        else if ev.event_code == GAMMA_UP && ev.value > 0 {
            Command::new("gamma_up.sh").output().expect("Failed to execute amixer");
        }
        else if ev.event_code == GAMMA_DOWN && ev.value > 0 {
            Command::new("gamma_dn.sh").output().expect("Failed to execute amixer");
        }
    }
    else if ev.event_code == EventCode::EV_SW(EV_SW::SW_HEADPHONE_INSERT) {
        let dest = match ev.value { 1 => "SPK", _ => "HP" };
        Command::new("amixer").args(&["-q", "sset", "'Playback Path'", dest]).output().expect("Failed to execute amixer");
        //blink1();
    }
    else if ev.event_code == EventCode::EV_KEY(EV_KEY::KEY_POWER) && ev.value == 1 {
        //blink2();
        Command::new("pause.sh").spawn().ok().expect("Failed to execute suspend process");
    }
    else if ev.event_code == VOLUME_UP  && ev.value > 0 {
        Command::new("amixer").args(&["-q", "sset", "Playback", "1%+"]).output().expect("Failed to execute amixer");
    }
    else if ev.event_code == VOLUME_DOWN  && ev.value > 0 {
        Command::new("amixer").args(&["-q", "sset", "Playback", "1%-"]).output().expect("Failed to execute amixer");
    }
    if ev.value == 0 {
        let code = &ev.event_code;

        if *code == BRIGHT_UP
            || *code == BRIGHT_UP2
            || *code == BRIGHT_DOWN
            || *code == BRIGHT_DOWN2
            || *code == VOL_UP
            || *code == VOL_DOWN
        {
            repeat_action.store(RepeatAction::None as u8, Ordering::Relaxed);
            repeat_active.store(false, Ordering::Relaxed);
        }
    }
}

fn process_event2(_dev: &Device, ev: &InputEvent, selectkey: bool) {
    /*println!("Event: time {}.{} type {} code {} value {} selectkey {}",
             ev.time.tv_sec,
             ev.time.tv_usec,
             ev.event_type,
             ev.event_code,
             ev.value,
             selectkey);*/

    if selectkey{
        if ev.event_code == EventCode::EV_KEY(EV_KEY::BTN_TRIGGER_HAPPY4) && ev.value == 1 {
            Command::new("speak_bat_life.sh").spawn().ok().expect("Failed to execute battery reading out loud");
        }
    }
}

fn main() -> io::Result<()> {
    let mut poll = Poll::new()?;
    let mut events = Events::with_capacity(1);
    let mut devs: Vec<Device> = Vec::new();
    let mut hotkey = false;
    let mut selectkey = false;
    let repeat_action = Arc::new(AtomicU8::new(RepeatAction::None as u8));
    let repeat_active = Arc::new(AtomicBool::new(false));
{
    let repeat_action = repeat_action.clone();
    let repeat_active = repeat_active.clone();

    thread::spawn(move || {
    loop {
        match unsafe {
            std::mem::transmute::<u8, RepeatAction>(
                repeat_action.load(Ordering::Relaxed),
            )
        } {
            RepeatAction::BrightUp => {
                let _ = Command::new("brightnessctl")
                    .args(&["s", "+2%"])
                    .output();
            }
            RepeatAction::BrightDown => {
                let _ = Command::new("brightnessctl")
                    .args(&["-n", "s", "2%-"])
                    .output();
            }
            RepeatAction::VolUp => {
                let _ = Command::new("amixer")
                    .args(&["-q", "sset", "Playback", "1%+"])
                    .output();
            }
            RepeatAction::VolDown => {
                let _ = Command::new("amixer")
                    .args(&["-q", "sset", "Playback", "1%-"])
                    .output();
            }
            RepeatAction::None => {
                // nothing to do
            }
        }

        thread::sleep(Duration::from_millis(120));
    }
    });
}

    let mut i = 0;
for s in ["/dev/input/event10", "/dev/input/event9", "/dev/input/event8", "/dev/input/event7", "/dev/input/event6", "/dev/input/event5", "/dev/input/event4", "/dev/input/event3", "/dev/input/event2", "/dev/input/event1", "/dev/input/event0"].iter() {
        if !Path::new(s).exists() {
            println!("Path {} doesn't exist", s);
            continue;
        }
        let fd = File::open(Path::new(s)).unwrap();
        let mut dev = Device::new().unwrap();
        poll.registry().register(&mut SourceFd(&fd.as_raw_fd()), Token(i), Interest::READABLE)?;
        dev.set_fd(fd)?;
        devs.push(dev);
        println!("Added {}", s);
        i += 1;
    }

    //Command::new("brightnessctl").arg("-I").output().expect("Failed to execute brightnessctl");

    loop {
        poll.poll(&mut events, None)?;

        for event in events.iter() {
            let dev = &mut devs[event.token().0];
            while dev.has_event_pending() {
                let e = dev.next_event(evdev_rs::ReadFlag::NORMAL);
                match e {
                    Ok(k) => {
                        let ev = &k.1;
                        if ev.event_code == HOTKEY {
                            hotkey = ev.value == 1;
                            //let grab = if hotkey { GrabMode::Grab } else { GrabMode::Ungrab };
                            //dev.grab(grab)?;
                        }
                        process_event(&dev, &ev, hotkey, &repeat_action, &repeat_active);
                        if ev.event_code == EventCode::EV_KEY(EV_KEY::BTN_TRIGGER_HAPPY1) {
                            selectkey = ev.value == 1 || ev.value == 2;
                        }
                        process_event2(&dev, &ev, selectkey)
                    },
                    _ => ()
                }
            }
        }
    }
}

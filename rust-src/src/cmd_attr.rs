use atty::Stream;
use libc::removexattr;
use log::{error};
use bch_bindgen::bcachefs;
use bch_bindgen::opt_set;
use bch_bindgen::fs::Fs;
use bch_bindgen::bkey::BkeySC;
use bch_bindgen::btree::BtreeTrans;
use bch_bindgen::btree::BtreeIter;
use bch_bindgen::btree::BtreeNodeIter;
use bch_bindgen::btree::BtreeIterFlags;
use clap::{Parser, Subcommand};
use std::ffi::CString;
use std::ffi::{CStr, OsStr, c_int, c_char};
use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use anyhow::anyhow;

#[derive(Parser, Debug)]
struct Cli {
    #[command(subcommand)]
    command: Command,
    /// Force color on/off. Default: autodetect tty
    #[arg(short, long, action = clap::ArgAction::Set, default_value_t=atty::is(Stream::Stdout))]
    colorize:   bool,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Set an attribute
    Add {},
    /// Unset an attribute
    Remove {
        /// Attributes
        #[arg(required(true), value_enum, value_parser, num_args = 1.., value_delimiter = ',')]
        attributes: Vec<String>,
        /// Path
        #[arg(required(true))]
        path: std::path::PathBuf,
    }
}

fn path_to_cstr<P: AsRef<std::path::Path>>(path: P) -> CString {
    let path_str_c = CString::new(path.as_ref().as_os_str().as_bytes()).unwrap();
    path_str_c
}

fn cmd_attr_inner(opt: Cli) -> anyhow::Result<()> {
    match opt.command {
        Command::Add {} => Ok(()),
        Command::Remove {attributes, path} => {
            let f = File::open(&path)?;
            let fd = f.as_raw_fd();
            let is_dir = f.metadata()?.is_dir();

            for attr in attributes {
                let a = format!("bcachefs.{}", attr);
                println!("removing {} from {:?}",a, &path);
                unsafe {
                    if 0 != removexattr(path_to_cstr(&path).as_ptr(), path_to_cstr(a).as_ptr())
                    {
                        return Err(anyhow!("removexattr error: {}", std::io::Error::last_os_error().raw_os_error().unwrap()));

                    }
                }
            };

            if is_dir {
                unsafe {
                    bcachefs::propagate_recurse(fd);
                }
            };

            Ok(())
        },
    }
}

#[no_mangle]
pub extern "C" fn cmd_attr(argc: c_int, argv: *const *const c_char) {
    let argv: Vec<_> = (0..argc)
        .map(|i| unsafe { CStr::from_ptr(*argv.add(i as usize)) })
        .map(|i| OsStr::from_bytes(i.to_bytes()))
        .collect();

    let opt = Cli::parse_from(argv);
    colored::control::set_override(opt.colorize);
    if let Err(e) = cmd_attr_inner(opt) {
        println!("{}",e );
        // error! doesn't show anything on my machine?
        error!("Fatal error: {}", e);
    }
}

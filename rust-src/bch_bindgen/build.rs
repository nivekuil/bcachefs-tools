
#[derive(Debug)]
pub struct Fix753 {}
impl bindgen::callbacks::ParseCallbacks for Fix753 {
    fn item_name(&self, original_item_name: &str) -> Option<String> {
        Some(original_item_name.trim_start_matches("Fix753_").to_owned())
    }
}

fn main() {
    use std::path::PathBuf;

    let out_dir: PathBuf = std::env::var_os("OUT_DIR")
        .expect("ENV Var 'OUT_DIR' Expected")
        .into();
    let top_dir: PathBuf = std::env::var_os("CARGO_MANIFEST_DIR")
        .expect("ENV Var 'CARGO_MANIFEST_DIR' Expected")
        .into();

    let libbcachefs_inc_dir = std::path::Path::new("../..");

    let _libbcachefs_dir = top_dir.join("libbcachefs").join("libbcachefs");
    let bindings = bindgen::builder()
        .header(
            top_dir
                .join("src")
                .join("libbcachefs_wrapper.h")
                .display()
                .to_string(),
        )
        .clang_arg(format!(
            "-I{}",
            libbcachefs_inc_dir.join("include").display()
        ))
        .clang_arg(format!("-I{}", libbcachefs_inc_dir.display()))
        .clang_arg("-DZSTD_STATIC_LINKING_ONLY")
        .clang_arg("-DNO_BCACHEFS_FS")
        .clang_arg("-D_GNU_SOURCE")
        .clang_arg("-fkeep-inline-functions")
        .derive_debug(true)
        .derive_default(true)
        .layout_tests(true)
        .default_enum_style(bindgen::EnumVariation::Rust {
            non_exhaustive: true,
        })
        .allowlist_function(".*bch2_.*")
        .allowlist_function("bio_.*")
        .allowlist_function("propagate_recurse")
        .allowlist_function("derive_passphrase")
        .allowlist_function("request_key")
        .allowlist_function("add_key")
        .allowlist_function("keyctl_search")
        .allowlist_function("match_string")
        .allowlist_function("printbuf.*")
        .blocklist_type("bch_extent_ptr")
        .blocklist_type("btree_node")
        .blocklist_type("bch_extent_crc32")
        .blocklist_type("rhash_lock_head")
        .blocklist_type("srcu_struct")
        .allowlist_var("BCH_.*")
        .allowlist_var("KEY_SPEC_.*")
        .allowlist_var("Fix753_FMODE_.*")
        .allowlist_var("bch.*")
        .allowlist_var("__BTREE_ITER.*")
        .allowlist_var("BTREE_ITER.*")
        .blocklist_item("bch2_bkey_ops")
        .allowlist_type("bch_.*")
        .allowlist_type("fsck_err_opts")
        .rustified_enum("fsck_err_opts")
        .allowlist_type("nonce")
        .no_debug("bch_replicas_padded")
        .newtype_enum("bch_kdf_types")
        .rustified_enum("bch_key_types")
        .opaque_type("gendisk")
        .opaque_type("gc_stripe")
        .opaque_type("open_bucket.*")
        .opaque_type("replicas_delta_list")
        .no_copy("btree_trans")
        .no_copy("printbuf")
        .no_partialeq("bkey")
        .no_partialeq("bpos")
        .generate_inline_functions(true)
        .parse_callbacks(Box::new(Fix753 {}))
        .generate()
        .expect("BindGen Generation Failiure: [libbcachefs_wrapper]");
    bindings
        .write_to_file(out_dir.join("bcachefs.rs"))
        .expect("Writing to output file failed for: `bcachefs.rs`");

    let keyutils = pkg_config::probe_library("libkeyutils").expect("Failed to find keyutils lib");
    let bindings = bindgen::builder()
        .header(
            top_dir
                .join("src")
                .join("keyutils_wrapper.h")
                .display()
                .to_string(),
        )
        .clang_args(
            keyutils
                .include_paths
                .iter()
                .map(|p| format!("-I{}", p.display())),
        )
        .generate()
        .expect("BindGen Generation Failiure: [Keyutils]");
    bindings
        .write_to_file(out_dir.join("keyutils.rs"))
        .expect("Writing to output file failed for: `keyutils.rs`");
}

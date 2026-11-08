use super::{IconAssets, IconPublisher, ICON_RGBA_LEN, MAX_ICON_KEY};
use std::{
    collections::HashSet,
    fs,
    io::Cursor,
    os::unix::fs::{MetadataExt, PermissionsExt},
    rc::Rc,
};

fn rgba(pixel: [u8; 4]) -> Vec<u8> {
    pixel.repeat(ICON_RGBA_LEN / 4)
}

#[test]
fn png_is_complete_rgba8_and_heartbeat_names_are_immutable() {
    let root = tempfile::tempdir().unwrap();
    let publisher = IconPublisher::new(root.path()).unwrap();
    let assets = &publisher.assets;
    let off = rgba([24, 95, 69, 255]);
    let on = rgba([52, 211, 153, 255]);
    let off_name = assets.publish(0x040, &off).unwrap();
    let on_name = assets.publish(0x140, &on).unwrap();
    assert_eq!(off_name, "linkr-state-040");
    assert_eq!(on_name, "linkr-state-140");
    let path = assets.directory.join(format!("{off_name}.png"));
    let encoded = fs::read(&path).unwrap();
    let metadata = fs::metadata(&path).unwrap();

    let mut decoder = png::Decoder::new(Cursor::new(&encoded))
        .read_info()
        .unwrap();
    let mut decoded = vec![0; decoder.output_buffer_size().unwrap()];
    let frame = decoder.next_frame(&mut decoded).unwrap();
    assert_eq!((frame.width, frame.height), (32, 32));
    assert_eq!(frame.color_type, png::ColorType::Rgba);
    assert_eq!(frame.bit_depth, png::BitDepth::Eight);
    assert_eq!(&decoded[..frame.buffer_size()], off);
    decoder.finish().unwrap();

    for _ in 0..8 {
        assert_eq!(assets.publish(0x040, &on).unwrap(), off_name);
        assert_eq!(assets.publish(0x140, &off).unwrap(), on_name);
    }
    assert_eq!(fs::read(&path).unwrap(), encoded);
    let after = fs::metadata(&path).unwrap();
    assert_eq!(after.ino(), metadata.ino());
    assert_eq!(after.modified().unwrap(), metadata.modified().unwrap());
    assert_eq!(fs::read_dir(&assets.directory).unwrap().count(), 2);
}

#[test]
fn names_are_bounded_and_out_of_range_keys_do_not_write_files() {
    let names: HashSet<_> = (0..=MAX_ICON_KEY)
        .map(|key| IconAssets::icon_name(key).unwrap())
        .collect();
    assert_eq!(names.len(), 512);
    assert!(names.contains("linkr-state-000"));
    assert!(names.contains("linkr-state-1ff"));
    let root = tempfile::tempdir().unwrap();
    let assets = IconAssets::new(root.path()).unwrap();
    for key in [0x200, u16::MAX] {
        assert!(IconAssets::icon_name(key).is_err());
        assert!(assets.publish(key, &rgba([0; 4])).is_err());
    }
    assert_eq!(fs::read_dir(&assets.directory).unwrap().count(), 0);
    assert!(!assets.published.borrow().iter().any(|published| *published));
}

#[test]
fn malformed_rgba_does_not_publish_or_poison_a_key() {
    let root = tempfile::tempdir().unwrap();
    let assets = IconAssets::new(root.path()).unwrap();
    for length in [0, ICON_RGBA_LEN - 1, ICON_RGBA_LEN + 1] {
        assert!(assets.publish(1, &vec![0; length]).is_err());
    }
    assert_eq!(fs::read_dir(&assets.directory).unwrap().count(), 0);
    assert!(!assets.published.borrow()[1]);
    assert_eq!(assets.publish(1, &rgba([0; 4])).unwrap(), "linkr-state-001");
    assert_eq!(fs::read_dir(&assets.directory).unwrap().count(), 1);
}

#[test]
fn failed_atomic_rename_cleans_temporary_file_and_can_retry() {
    let root = tempfile::tempdir().unwrap();
    let assets = IconAssets::new(root.path()).unwrap();
    let final_path = assets.directory.join("linkr-state-002.png");
    fs::create_dir(&final_path).unwrap();
    assert!(assets.publish(2, &rgba([0; 4])).is_err());
    assert!(final_path.is_dir());
    assert_eq!(fs::read_dir(&assets.directory).unwrap().count(), 1);
    assert!(!assets.published.borrow()[2]);
    fs::remove_dir(&final_path).unwrap();
    assets.publish(2, &rgba([0; 4])).unwrap();
    assert!(final_path.is_file());
    assert_eq!(fs::read_dir(&assets.directory).unwrap().count(), 1);
}

#[test]
fn directory_is_unique_private_and_lives_until_last_owner() {
    let root = tempfile::tempdir().unwrap();
    let legacy = root.path().join("tray-icons");
    fs::create_dir(&legacy).unwrap();
    let old_slot = legacy.join("tray-icon-test-0.png");
    fs::write(&old_slot, b"old slot").unwrap();
    let first = IconPublisher::new(root.path()).unwrap();
    let second = IconPublisher::new(root.path()).unwrap();
    let directory = first.assets.directory.clone();
    let other_directory = second.assets.directory.clone();
    assert_ne!(directory, other_directory);
    assert!(directory.is_absolute());
    assert_eq!(
        fs::metadata(&directory).unwrap().permissions().mode() & 0o777,
        0o700
    );
    first.assets.publish(3, &rgba([0; 4])).unwrap();
    second.assets.publish(3, &rgba([255; 4])).unwrap();
    // A tray owns the same Rc without requiring GTK initialization in this test.
    let tray_owner = Rc::clone(&first.assets);
    drop(first);
    assert!(directory.join("linkr-state-003.png").is_file());
    drop(tray_owner);
    assert!(!directory.exists());
    assert!(other_directory.join("linkr-state-003.png").is_file());
    assert_eq!(fs::read(&old_slot).unwrap(), b"old slot");
    drop(second);
    assert!(!other_directory.exists());
    assert!(root.path().is_dir());
    assert_eq!(fs::read(&old_slot).unwrap(), b"old slot");
}

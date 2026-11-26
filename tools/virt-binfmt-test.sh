#!/bin/sh
# virt-binfmt-test.sh <vmlinux> [meson build dir] : boot an m68k "virt" kernel
# with CONFIG_BINFMT_MISC=y under qemu-system-m68k, register amigafmt and
# run hunk executables directly (./hello), as root and as a user.
#
# The kernel: virt_defconfig with BINFMT_MISC, BLK_DEV_INITRD and
# DEVTMPFS_MOUNT enabled, e.g.
#   make ARCH=m68k CROSS_COMPILE=m68k-linux-gnu- O=/tmp/afvirt virt_defconfig
#   scripts/config --file /tmp/afvirt/.config --enable BINFMT_MISC \
#       --enable BLK_DEV_INITRD --enable DEVTMPFS_MOUNT
#   make -C /tmp/afvirt ARCH=m68k CROSS_COMPILE=m68k-linux-gnu- olddefconfig vmlinux
# $INITRAMFS is a gzipped cpio archive with a static busybox at /bin/busybox.
set -e
K=$1
O=${2:-build}
here=$(cd "$(dirname "$0")/.." && pwd)
R=$O/virt-root
rm -rf "$R"; mkdir -p "$R"
[ -n "${INITRAMFS:-}" ] || { echo "set INITRAMFS to a busybox initramfs" >&2; exit 1; }
zcat "$INITRAMFS" | (cd "$R" && cpio -id 2>/dev/null)
mkdir -p "$R/usr/local/bin" "$R/root/t" "$R/etc"
${CROSS:-m68k-linux-gnu-}strip -o "$R/usr/local/bin/amigafmt" "$O/amigafmt"
cp "$here/tools/register.sh" "$R/"
for t in hello targs tunimpl tmath; do cp "$O/tests/$t" "$R/root/t/"; done
chmod +x "$R/root/t/"*
echo 'root:x:0:0:root:/root:/bin/sh' > "$R/etc/passwd"
echo 'u:x:1000:1000:u:/tmp:/bin/sh' >> "$R/etc/passwd"
printf 'root:x:0:\nu:x:1000:\n' > "$R/etc/group"
cat > "$R/init" <<'EOI'
#!/bin/busybox sh
/bin/busybox --install -s
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "=== binfmt_misc test, kernel $(uname -r)"
sh /register.sh /usr/local/bin/amigafmt
cd /root/t
echo "=== ./hello"; AMIGAFMT_TRACE=1 ./hello 2>&1 | grep -v library; echo "rc=$?"
echo "=== ./targs one two"; ./targs one two; echo "rc=$?"
echo "=== ./tmath"; ./tmath; echo "rc=$?"
echo "=== ./tunimpl"; ./tunimpl; echo "rc=$?"
echo "=== as a user"; su u -c "AMIGAFMT_TRACE=1 ./hello 2>&1 | grep address"
echo "=== unregistered"; sh /register.sh -u; ./hello; echo "rc=$?"
echo "=== DONE"
poweroff -f
EOI
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc 2>/dev/null | gzip) > "$O/virt-initrd.gz"
timeout 120 qemu-system-m68k -M virt -m 256 -nographic -kernel "$K" \
	-initrd "$O/virt-initrd.gz" -append console=ttyGF0 2>&1 |
	sed -n '/=== binfmt/,/=== DONE/p'

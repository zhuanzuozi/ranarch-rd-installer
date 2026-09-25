# Maintainer: RanArch Team
# PKGBUILD for ranarch-rd-installer — direct .deb/.rpm installer for Arch Linux.

pkgname=ranarch-rd-installer
pkgver=0.1.0
pkgrel=1
pkgdesc="Direct .deb/.rpm binary package installer for Arch Linux and derivatives"
arch=('x86_64' 'aarch64')
url="https://github.com/ranarch/ranarch-rd-installer"
license=('GPL-3.0-or-later')
depends=(
    'libarchive'      # ar/tar/cpio extraction
    'sqlite'          # tracking database
    'gpgme'           # GPG signature verification
    'pacman'          # libalpm for local package queries
    'bubblewrap'      # sandbox trial install (optional but recommended)
    'gnupg'           # gpg command-line for key generation
)
makedepends=(
    'cmake'
    'gcc'
    'pkgconf'
)
source=("$pkgname-$pkgver.tar.gz::https://github.com/ranarch/ranarch-rd-installer/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')  # Replace with actual hash on release.

build() {
    cd "$srcdir/$pkgname-$pkgver"
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j"$(nproc)"
}

check() {
    cd "$srcdir/$pkgname-$pkgver/build"
    ctest --output-on-failure
}

package() {
    cd "$srcdir/$pkgname-$pkgver"
    DESTDIR="$pkgdir" cmake --install build

    # Create runtime directories
    install -d -m 0755 "$pkgdir/var/lib/ranarch"
    install -d -m 0755 "$pkgdir/var/log/ranarch"
    install -d -m 0755 "$pkgdir/run/ranarch"
    install -d -m 0755 "$pkgdir/usr/share/ranarch/keyring"
    install -d -m 0755 "$pkgdir/var/lib/ranarch/keyring"
    install -d -m 0755 "$pkgdir/var/lib/ranarch/staging"
}

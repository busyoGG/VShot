#!/bin/bash

set -ueo pipefail

# Populate the initial `~/rpmbuild` directory just in case.
rpmdev-setuptree

cp *.diff ~/rpmbuild/SOURCES
# Removing %dist makes the SRPM path distro independent.
for i in $(ls *.spec) ; do rpmbuild --undefine=dist --undefine=_disable_source_fetch -bs $i ; done

# Not typing this path 50+ times.
pushd ~/rpmbuild/SRPMS
# With --chain, mock will save the previously built packages
# in a local repo that can be queried to fulfill dependencies
# of later packages, so the order matters here.
mock --chain --enable-network \
./rust-k9-0.12.0-1.src.rpm \
./rust-roxmltree-0.20.0-1.src.rpm \
./rust-fontconfig-parser-0.5.8-1.src.rpm \
./rust-fontdb-0.24.0-1.src.rpm \
./rust-arbtest-0.3.2-1.src.rpm \
./rust-polycool-0.4.0-1.src.rpm \
./rust-kurbo-0.13.1-1.src.rpm \
./rust-hayro-ccitt-0.3.0-1.src.rpm \
./rust-hayro-jbig2-0.3.0-1.src.rpm \
./rust-md5-0.8.1-1.src.rpm \
./rust-hayro-syntax-0.7.2-1.src.rpm \
./rust-hayro-postscript-0.1.0-1.src.rpm \
./rust-hayro-cmap-0.1.0-1.src.rpm \
./rust-font-types-0.11.3-1.src.rpm \
./rust-read-fonts-0.39.2-1.src.rpm \
./rust-skrifa-0.42.1-1.src.rpm \
./rust-hayro-interpret-0.7.0-1.src.rpm \
./rust-novtb-0.1.13-1.src.rpm \
./rust-pic-scale-0.7.12-1.src.rpm \
./rust-color-0.3.3-1.src.rpm \
./rust-linebender_resource_handle-0.1.1-1.src.rpm \
./rust-peniko-0.6.1-1.src.rpm \
./rust-guillotiere-0.7.0-1.src.rpm \
./rust-vello_common-0.0.8-1.src.rpm \
./rust-glifo-0.1.0-1.src.rpm \
./rust-vello_cpu-0.0.8-1.src.rpm \
./rust-hayro-0.7.1-1.src.rpm \
./rust-safe_arch-1.2.0-1.src.rpm \
./rust-wide-1.7.1-1.src.rpm \
./rust-simba-0.10.2-1.src.rpm \
./rust-nalgebra-0.35.0-1.src.rpm \
./rust-imageproc-0.27.0-1.src.rpm \
./rust-target-features-0.1.6-1.src.rpm \
./rust-multiversion-macros-0.8.0-1.src.rpm \
./rust-multiversion-0.8.0-1.src.rpm \
./rust-ndarray-0.17.2-1.src.rpm \
./rust-clipper2-rust-1.2.0-1.src.rpm \
./rust-monostate-impl-0.1.18-1.src.rpm \
./rust-monostate-0.1.18-1.src.rpm \
./rust-daachorse-3.0.3-1.src.rpm \
./rust-tokenizers-0.23.2-1.src.rpm \
./rust-darling_core-0.24.1-1.src.rpm \
./rust-darling_macro-0.24.1-1.src.rpm \
./rust-darling-0.24.1-1.src.rpm \
./rust-oar-ocr-derive-0.9.2-1.src.rpm \
./rust-ort-sys-2.0.0~rc.13-1.src.rpm \
./rust-liblzma-sys-0.4.9-1.src.rpm \
./rust-liblzma-0.4.8-1.src.rpm \
./rust-lzma-rust2-0.15.8-1.src.rpm \
./rust-hmac-sha256-1.1.15-1.src.rpm \
./rust-ort-2.0.0~rc.13-1.src.rpm \
./rust-oar-ocr-core-0.9.2-1.src.rpm \
./rust-avif-parse-2.1.0-1.src.rpm \
./rust-avif-serialize-0.8.9-1.src.rpm \
./rust-oar-ocr-0.9.2-1.src.rpm \
./vshot-0.1.10-1.src.rpm
popd

# COPR make_srpm entry point: builds the source RPM from this checkout.
#
#   make -f .copr/Makefile srpm outdir=/tmp/srpm
#
# Version comes from the nearest v* tag. Commits past it become 0.2.41^3.gabc1234,
# which sorts after 0.2.41 and before 0.2.42.

top := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
outdir ?= $(top)/build/srpm
git := git -C $(top) -c safe.directory='*'

srpm:
	command -v git >/dev/null && command -v rpmbuild >/dev/null || dnf -y install git rpm-build
	set -e; \
	desc=$$($(git) describe --tags --match 'v*' | sed 's/^v//'); \
	ver=$$(echo "$$desc" | sed 's/-\([0-9]*\)-g/^\1.g/'); \
	work=$$(mktemp -d); \
	$(git) archive --prefix=tether-$$ver/ -o $$work/tether-$$ver.tar.gz HEAD; \
	sed -e "s/@VERSION@/$$ver/" -e "s/@TETHER_VERSION@/$$desc/" $(top)/packaging/rpm/tether.spec > $$work/tether.spec; \
	rpmbuild -bs --define "_sourcedir $$work" --define "_srcrpmdir $(outdir)" $$work/tether.spec

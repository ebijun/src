# Helper targets for the vc4-drmkms development branch.
#
# Run from the NetBSD source tree root:
#   make -f Documentation/vc4-drmkms/Makefile check-current
#   make -f Documentation/vc4-drmkms/Makefile sync-current
#
# check-current only fetches and displays upstream changes.
# sync-current merges origin/trunk and refuses to run with a dirty tree.
# Build and test before pushing; pushing is intentionally not automated.

BRANCH?=vc4-drmkms
UPSTREAM?=origin/trunk

.PHONY: check-current sync-current status

check-current:
	@test "$$(git branch --show-current)" = "$(BRANCH)" || { \
		echo "Error: switch to $(BRANCH) first." >&2; exit 1; }
	@git fetch origin
	@echo "Commits in $(UPSTREAM) not yet in $(BRANCH):"
	@git log --oneline --decorate HEAD..$(UPSTREAM)
	@echo
	@echo "Change summary:"
	@git diff --stat HEAD...$(UPSTREAM)

sync-current:
	@test "$$(git branch --show-current)" = "$(BRANCH)" || { \
		echo "Error: switch to $(BRANCH) first." >&2; exit 1; }
	@test -z "$$(git status --porcelain)" || { \
		echo "Error: working tree is not clean; commit or safely stash changes first." >&2; exit 1; }
	@git fetch origin
	@git merge --no-ff $(UPSTREAM)

status:
	@git status -sb
	@git log --oneline --decorate -5

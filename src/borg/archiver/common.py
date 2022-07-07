import argparse
import functools
import textwrap

import borg
from ..archive import Archive
from ..constants import *
from ..cache import Cache, assert_secure
from ..helpers import Error
from ..helpers import Manifest
from ..remote import RemoteRepository
from ..repository import Repository

from ..nanorst import rst_to_terminal

from ..logger import create_logger

logger = create_logger(__name__)


def argument(args, str_or_bool):
    """If bool is passed, return it. If str is passed, retrieve named attribute from args."""
    if isinstance(str_or_bool, str):
        return getattr(args, str_or_bool)
    if isinstance(str_or_bool, (list, tuple)):
        return any(getattr(args, item) for item in str_or_bool)
    return str_or_bool


def get_repository(location, *, create, exclusive, lock_wait, lock, append_only, make_parent_dirs, storage_quota, args):
    if location.proto == "ssh":
        repository = RemoteRepository(
            location,
            create=create,
            exclusive=exclusive,
            lock_wait=lock_wait,
            lock=lock,
            append_only=append_only,
            make_parent_dirs=make_parent_dirs,
            args=args,
        )

    else:
        repository = Repository(
            location.path,
            create=create,
            exclusive=exclusive,
            lock_wait=lock_wait,
            lock=lock,
            append_only=append_only,
            make_parent_dirs=make_parent_dirs,
            storage_quota=storage_quota,
        )
    return repository


def compat_check(*, create, manifest, key, cache, compatibility, decorator_name):
    if not create and (manifest or key or cache):
        if compatibility is None:
            raise AssertionError(f"{decorator_name} decorator used without compatibility argument")
        if type(compatibility) is not tuple:
            raise AssertionError(f"{decorator_name} decorator compatibility argument must be of type tuple")
    else:
        if compatibility is not None:
            raise AssertionError(
                f"{decorator_name} called with compatibility argument, " f"but would not check {compatibility!r}"
            )
        if create:
            compatibility = Manifest.NO_OPERATION_CHECK
    return compatibility


def with_repository(
    fake=False,
    invert_fake=False,
    create=False,
    lock=True,
    exclusive=False,
    manifest=True,
    cache=False,
    secure=True,
    compatibility=None,
):
    """
    Method decorator for subcommand-handling methods: do_XYZ(self, args, repository, …)

    If a parameter (where allowed) is a str the attribute named of args is used instead.
    :param fake: (str or bool) use None instead of repository, don't do anything else
    :param create: create repository
    :param lock: lock repository
    :param exclusive: (str or bool) lock repository exclusively (for writing)
    :param manifest: load manifest and key, pass them as keyword arguments
    :param cache: open cache, pass it as keyword argument (implies manifest)
    :param secure: do assert_secure after loading manifest
    :param compatibility: mandatory if not create and (manifest or cache), specifies mandatory feature categories to check
    """
    # Note: with_repository decorator does not have a "key" argument (yet?)
    compatibility = compat_check(
        create=create,
        manifest=manifest,
        key=manifest,
        cache=cache,
        compatibility=compatibility,
        decorator_name="with_repository",
    )

    # To process the `--bypass-lock` option if specified, we need to
    # modify `lock` inside `wrapper`. Therefore we cannot use the
    # `nonlocal` statement to access `lock` as modifications would also
    # affect the scope outside of `wrapper`. Subsequent calls would
    # only see the overwritten value of `lock`, not the original one.
    # The solution is to define a place holder variable `_lock` to
    # propagate the value into `wrapper`.
    _lock = lock

    def decorator(method):
        @functools.wraps(method)
        def wrapper(self, args, **kwargs):
            location = getattr(args, "location")
            if not location.valid:  # location always must be given
                raise Error("missing repository, please use --repo or BORG_REPO env var!")
            lock = getattr(args, "lock", _lock)
            append_only = getattr(args, "append_only", False)
            storage_quota = getattr(args, "storage_quota", None)
            make_parent_dirs = getattr(args, "make_parent_dirs", False)
            if argument(args, fake) ^ invert_fake:
                return method(self, args, repository=None, **kwargs)

            repository = get_repository(
                location,
                create=create,
                exclusive=argument(args, exclusive),
                lock_wait=self.lock_wait,
                lock=lock,
                append_only=append_only,
                make_parent_dirs=make_parent_dirs,
                storage_quota=storage_quota,
                args=args,
            )

            with repository:
                if repository.version not in (2,):
                    raise Error(
                        "This borg version only accepts version 2 repos for -r/--repo. "
                        "You can use 'borg transfer' to copy archives from old to new repos."
                    )
                if manifest or cache:
                    kwargs["manifest"], kwargs["key"] = Manifest.load(repository, compatibility)
                    if "compression" in args:
                        kwargs["key"].compressor = args.compression.compressor
                    if secure:
                        assert_secure(repository, kwargs["manifest"], self.lock_wait)
                if cache:
                    with Cache(
                        repository,
                        kwargs["key"],
                        kwargs["manifest"],
                        progress=getattr(args, "progress", False),
                        lock_wait=self.lock_wait,
                        cache_mode=getattr(args, "files_cache_mode", FILES_CACHE_MODE_DISABLED),
                        consider_part_files=getattr(args, "consider_part_files", False),
                        iec=getattr(args, "iec", False),
                    ) as cache_:
                        return method(self, args, repository=repository, cache=cache_, **kwargs)
                else:
                    return method(self, args, repository=repository, **kwargs)

        return wrapper

    return decorator


def with_other_repository(manifest=False, key=False, cache=False, compatibility=None):
    """
    this is a simplified version of "with_repository", just for the "other location".

    the repository at the "other location" is intended to get used as a **source** (== read operations).
    """

    compatibility = compat_check(
        create=False,
        manifest=manifest,
        key=key,
        cache=cache,
        compatibility=compatibility,
        decorator_name="with_other_repository",
    )

    def decorator(method):
        @functools.wraps(method)
        def wrapper(self, args, **kwargs):
            location = getattr(args, "other_location")
            if not location.valid:  # nothing to do
                return method(self, args, **kwargs)

            repository = get_repository(
                location,
                create=False,
                exclusive=True,
                lock_wait=self.lock_wait,
                lock=True,
                append_only=False,
                make_parent_dirs=False,
                storage_quota=None,
                args=args,
            )

            with repository:
                if repository.version not in (1, 2):
                    raise Error("This borg version only accepts version 1 or 2 repos for --other-repo.")
                kwargs["other_repository"] = repository
                if manifest or key or cache:
                    manifest_, key_ = Manifest.load(repository, compatibility)
                    assert_secure(repository, manifest_, self.lock_wait)
                    if manifest:
                        kwargs["other_manifest"] = manifest_
                    if key:
                        kwargs["other_key"] = key_
                if cache:
                    with Cache(
                        repository,
                        key_,
                        manifest_,
                        progress=False,
                        lock_wait=self.lock_wait,
                        cache_mode=getattr(args, "files_cache_mode", FILES_CACHE_MODE_DISABLED),
                        consider_part_files=getattr(args, "consider_part_files", False),
                        iec=getattr(args, "iec", False),
                    ) as cache_:
                        kwargs["other_cache"] = cache_
                        return method(self, args, **kwargs)
                else:
                    return method(self, args, **kwargs)

        return wrapper

    return decorator


def with_archive(method):
    @functools.wraps(method)
    def wrapper(self, args, repository, key, manifest, **kwargs):
        archive_name = getattr(args, "name", None)
        assert archive_name is not None
        archive = Archive(
            repository,
            key,
            manifest,
            archive_name,
            numeric_ids=getattr(args, "numeric_ids", False),
            noflags=getattr(args, "nobsdflags", False) or getattr(args, "noflags", False),
            noacls=getattr(args, "noacls", False),
            noxattrs=getattr(args, "noxattrs", False),
            cache=kwargs.get("cache"),
            consider_part_files=args.consider_part_files,
            log_json=args.log_json,
            iec=args.iec,
        )
        return method(self, args, repository=repository, manifest=manifest, key=key, archive=archive, **kwargs)

    return wrapper


class Highlander(argparse.Action):
    """make sure some option is only given once"""

    def __call__(self, parser, namespace, values, option_string=None):
        if getattr(namespace, self.dest, None) != self.default:
            raise argparse.ArgumentError(self, "There can be only one.")
        setattr(namespace, self.dest, values)


# You can use :ref:`xyz` in the following usage pages. However, for plain-text view,
# e.g. through "borg ... --help", define a substitution for the reference here.
# It will replace the entire :ref:`foo` verbatim.
rst_plain_text_references = {
    "a_status_oddity": '"I am seeing ‘A’ (added) status for a unchanged file!?"',
    "separate_compaction": '"Separate compaction"',
    "list_item_flags": '"Item flags"',
    "borg_patterns": '"borg help patterns"',
    "borg_placeholders": '"borg help placeholders"',
    "key_files": "Internals -> Data structures and file formats -> Key files",
    "borg_key_export": "borg key export --help",
}


def process_epilog(epilog):
    epilog = textwrap.dedent(epilog).splitlines()
    try:
        mode = borg.doc_mode
    except AttributeError:
        mode = "command-line"
    if mode in ("command-line", "build_usage"):
        epilog = [line for line in epilog if not line.startswith(".. man")]
    epilog = "\n".join(epilog)
    if mode == "command-line":
        epilog = rst_to_terminal(epilog, rst_plain_text_references)
    return epilog

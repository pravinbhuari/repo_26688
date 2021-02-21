from typing import FrozenSet, Set, NamedTuple, Tuple, Mapping, Dict, List, Iterator, Callable, Any

from .helpers import StableDict

API_VERSION: str


class PropDict:
    VALID_KEYS: Set[str] = ...
    def __init__(self, data_dict: dict = None, internal_dict: dict = None, **kw) -> None: ...
    def as_dict(self) -> StableDict: ...
    def get(self, key: str, default: Any = None) -> Any: ...
    def update(self, d: dict) -> None: ...
    def update_internal(self, d: dict) -> None: ...
    def __contains__(self, key: str) -> bool: ...
    def __eq__(self, other: object) -> bool: ...


class ArchiveItem(PropDict):
    @property
    def version(self) -> int: ...
    @version.setter
    def version(self, val: int) -> None: ...
    @property
    def name(self) -> str: ...
    @name.setter
    def name(self, val: str) -> None: ...
    @property
    def time(self) -> str: ...
    @time.setter
    def time(self, val: str) -> None: ...
    @property
    def time_end(self) -> str: ...
    @time_end.setter
    def time_end(self, val: str) -> None: ...
    @property
    def username(self) -> str: ...
    @username.setter
    def username(self, val: str) -> None: ...
    @property
    def hostname(self) -> str: ...
    @hostname.setter
    def hostname(self, val: str) -> None: ...
    @property
    def comment(self) -> str: ...
    @comment.setter
    def comment(self, val: str) -> None: ...
    @property
    def chunker_params(self) -> Tuple: ...
    @chunker_params.setter
    def chunker_params(self, val: Tuple) -> None: ...
    @property
    def cmdline(self) -> List[str]: ...
    @cmdline.setter
    def cmdline(self, val: List[str]) -> None: ...
    @property
    def recreate_cmdline(self) -> List[str]: ...
    @recreate_cmdline.setter
    def recreate_cmdline(self, val: List[str]) -> None: ...

    @property
    def recreate_args(self) -> Any: ...
    @recreate_args.setter
    def recreate_args(self, val: Any) -> None: ...
    @property
    def recreate_partial_chunks(self) -> Any: ...
    @recreate_partial_chunks.setter
    def recreate_partial_chunks(self, val: Any) -> None: ...
    @property
    def recreate_source_id(self) -> Any: ...
    @recreate_source_id.setter
    def recreate_source_id(self, val: Any) -> None: ...

    @property
    def nfiles(self) -> int: ...
    @nfiles.setter
    def nfiles(self, val: int) -> None: ...
    @property
    def nfiles_parts(self) -> int: ...
    @nfiles_parts.setter
    def nfiles_parts(self, val: int) -> None: ...
    @property
    def size(self) -> int: ...
    @size.setter
    def size(self, val: int) -> None: ...
    @property
    def size_parts(self) -> int: ...
    @size_parts.setter
    def size_parts(self, val: int) -> None: ...
    @property
    def csize(self) -> int: ...
    @csize.setter
    def csize(self, val: int) -> None: ...
    @property
    def csize_parts(self) -> int: ...
    @csize_parts.setter
    def csize_parts(self, val: int) -> None: ...

    @property
    def items(self) -> List: ...
    @items.setter
    def items(self, val: List) -> None: ...


class ChunkListEntry(NamedTuple):
    id: bytes
    size: int
    csize: int


class Item(PropDict):
    @property
    def path(self) -> str: ...
    @path.setter
    def path(self, val: str) -> None: ...
    @property
    def source(self) -> str: ...
    @source.setter
    def source(self, val: str) -> None: ...

    def is_dir(self) -> bool: ...
    def is_link(self) -> bool: ...
    def _is_type(self, typetest: Callable) -> bool: ...

    @classmethod
    def create_deleted(self, path) -> Item: ...

    @classmethod
    def from_optr(self, optr: Any) -> Item: ...
    def to_optr(self) -> Any: ...

    @property
    def atime(self) -> int: ...
    @atime.setter
    def atime(self, val: int) -> None: ...
    @property
    def ctime(self) -> int: ...
    @ctime.setter
    def ctime(self, val: int) -> None: ...
    @property
    def mtime(self) -> int: ...
    @mtime.setter
    def mtime(self, val: int) -> None: ...
    @property
    def birthtime(self) -> int: ...
    @birthtime.setter
    def birthtime(self, val: int) -> None: ...

    @property
    def xattrs(self) -> StableDict: ...
    @xattrs.setter
    def xattrs(self, val: StableDict) -> None: ...

    @property
    def acl_access(self) -> bytes: ...
    @acl_access.setter
    def acl_access(self, val: bytes) -> None: ...
    @property
    def acl_default(self) -> bytes: ...
    @acl_default.setter
    def acl_default(self, val: bytes) -> None: ...
    @property
    def acl_extended(self) -> bytes: ...
    @acl_extended.setter
    def acl_extended(self, val: bytes) -> None: ...
    @property
    def acl_nfs4(self) -> bytes: ...
    @acl_nfs4.setter
    def acl_nfs4(self, val: bytes) -> None: ...

    @property
    def bsdflags(self) -> int: ...
    @bsdflags.setter
    def bsdflags(self, val: int) -> None: ...

    @property
    def chunks(self) -> List: ...
    @chunks.setter
    def chunks(self, val: List) -> None: ...
    @property
    def chunks_healthy(self) -> List: ...
    @chunks_healthy.setter
    def chunks_healthy(self, val: List) -> None: ...

    @property
    def deleted(self) -> bool: ...
    @deleted.setter
    def deleted(self, val: bool) -> None: ...
    @property
    def hardlink_master(self) -> bool: ...
    @hardlink_master.setter
    def hardlink_master(self, val: bool) -> None: ...

    @property
    def uid(self) -> int: ...
    @uid.setter
    def uid(self, val: int) -> None: ...
    @property
    def gid(self) -> int: ...
    @gid.setter
    def gid(self, val: int) -> None: ...
    @property
    def user(self) -> str: ...
    @user.setter
    def user(self, val: str) -> None: ...
    @property
    def group(self) -> str: ...
    @group.setter
    def group(self, val: str) -> None: ...
    @property
    def mode(self) -> int: ...
    @mode.setter
    def mode(self, val: int) -> None: ...
    @property
    def rdev(self) -> int: ...
    @rdev.setter
    def rdev(self, val: int) -> None: ...
    @property
    def nlink(self) -> int: ...
    @nlink.setter
    def nlink(self, val: int) -> None: ...

    @property
    def size(self) -> int: ...
    @size.setter
    def size(self, val: int) -> None: ...
    def get_size(self, hardlink_masters = ..., memorize: bool = ..., compressed: bool = ...,
                 from_chunks: bool = ..., consider_ids: List[bytes] = ...) -> int: ...

    @property
    def part(self) -> int: ...
    @part.setter
    def part(self, val: int) -> None: ...


class ManifestItem(PropDict):
    @property
    def version(self) -> int: ...
    @version.setter
    def version(self, val: int) -> None: ...
    @property
    def timestamp(self) -> str: ...
    @timestamp.setter
    def timestamp(self, val: str) -> None: ...
    @property
    def archives(self) -> Mapping[bytes, dict]: ...
    @archives.setter
    def archives(self, val: Mapping[bytes, dict]) -> None: ...
    @property
    def config(self) -> Dict: ...
    @config.setter
    def config(self, val: Dict) -> None: ...
    @property
    def item_keys(self) -> Tuple: ...
    @item_keys.setter
    def item_keys(self, val: Tuple) -> None: ...


class ItemDiff:
    def __init__(self, *args, **kwargs) -> None: ...
    @staticmethod
    def _chunk_content_equal(c1: Iterator, c2: Iterator) -> bool: ...


class Key(PropDict):
    @property
    def version(self) -> int: ...
    @version.setter
    def version(self, val: int) -> None: ...
    @property
    def chunk_seed(self) -> int: ...
    @chunk_seed.setter
    def chunk_seed(self, val: int) -> None: ...
    @property
    def tam_required(self) -> bool: ...
    @tam_required.setter
    def tam_required(self, val: bool) -> None: ...
    @property
    def enc_hmac_key(self) -> bytes: ...
    @enc_hmac_key.setter
    def enc_hmac_key(self, val: bytes) -> None: ...
    @property
    def enc_key(self) -> bytes: ...
    @enc_key.setter
    def enc_key(self, val: bytes) -> None: ...
    @property
    def id_key(self) -> bytes: ...
    @id_key.setter
    def id_key(self, val: bytes) -> None: ...
    @property
    def repository_id(self) -> bytes: ...
    @repository_id.setter
    def repository_id(self, val: bytes) -> None: ...


class EncryptedKey(PropDict):
    @property
    def version(self) -> int: ...
    @version.setter
    def version(self, val: int) -> None: ...
    @property
    def algorithm(self) -> str: ...
    @algorithm.setter
    def algorithm(self, val: str) -> None: ...
    @property
    def salt(self) -> bytes: ...
    @salt.setter
    def salt(self, val: bytes) -> None: ...
    @property
    def iterations(self) -> int: ...
    @iterations.setter
    def iterations(self, val: int) -> None: ...
    @property
    def data(self) -> bytes: ...
    @data.setter
    def data(self, val: bytes) -> None: ...
    @property
    def hash(self) -> bytes: ...
    @hash.setter
    def hash(self, val: bytes) -> None: ...

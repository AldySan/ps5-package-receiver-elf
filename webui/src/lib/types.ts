/* One entry of the PC's /catalog (PKG Sender "Publish library" JSON). */
export interface CatalogEntry {
  id: string;
  title: string;
  titleId?: string;
  platform?: string; /* "PS5" | "PS4" | "" */
  version?: string;
  role?: string; /* Game | Patch | DLC */
  format?: string; /* pkg | exfat | ffpkg | ffpfsc */
  familyKey?: string;
  file?: string;
  size?: number;
  sizeText?: string;
  hasIcon?: boolean;
}

/* GET /api/status */
export interface StatusReply {
  busy: boolean;
  active: number;
  pull: boolean;
  pullName: string;
  pullGot: number;
  pullWant: number;
  pullPaused: boolean;
  /* outcome of the last finished install. Newer payloads only: absent
   * on older ones, where "idle" was all the page could ever see. The
   * page matches lastUrl against the install it queued. lastFinal is
   * false for a direct install the console merely ACCEPTED (its
   * downloader owns the job and never reports back) — the page must
   * not call that "installed", only a final record is a verdict. */
  lastOk?: boolean;
  lastFinal?: boolean;
  lastUrl?: string;
  lastName?: string;
  lastErr?: string;
}

/* GET /api/version */
export interface VersionReply {
  build: string;
}

/* GET /api/pc — last PC that announced itself over UDP */
export interface PcReply {
  pc: string;
  age: number; /* seconds; -1 = none */
}

/* GET /api/space */
export interface SpaceReply {
  free: number; /* -1 when statvfs failed */
  total: number;
}

/* GET /api/files/stat?path=... */
export interface StatReply {
  exists: boolean;
  size: number;
}

/* POST /api/stage/clear — delete leftover staged packages. busy is
 * true when a staged install is running (its file is live and stays). */
export interface StageClearReply {
  ok: boolean;
  files?: number;
  bytes?: number;
  busy?: boolean;
}

export type MsgKind = "info" | "ok" | "err";

export interface Msg {
  text: string;
  kind: MsgKind;
}

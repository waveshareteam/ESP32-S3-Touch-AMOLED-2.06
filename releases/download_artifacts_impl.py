#!/usr/bin/env python3
"""Download and extract firmware artifacts from a GitHub Actions run."""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import stat
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath


DEFAULT_REPO = "waveshareteam/ESP32-S3-Touch-AMOLED-2.06"
DEFAULT_WORKFLOW = "examples.yml"
DEFAULT_PATTERN = "firmware-*"
DEFAULT_OUTPUT_DIR = Path(__file__).resolve().parent / "downloads"
GITHUB_API_ROOT = "https://api.github.com/"
USER_AGENT = "waveshare-firmware-artifact-downloader"


def slugify(value: str) -> str:
    value = value.strip().replace("\\", "/")
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value)
    value = re.sub(r"-+", "-", value).strip("-")
    return value or "artifact"


def run_text(command: list[str]) -> str | None:
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    value = result.stdout.strip()
    return value or None


def gh_available() -> bool:
    return shutil.which("gh") is not None


def mask_secret(value: str) -> str:
    value = re.sub(r"(gh[pousr]_[A-Za-z0-9_]+)", "***", value)
    value = re.sub(r"(github_pat_[A-Za-z0-9_]+)", "***", value)
    return value


def github_error_message(exc: urllib.error.HTTPError) -> str | None:
    try:
        body = exc.read().decode("utf-8", errors="replace")
    except Exception:
        return None
    if not body:
        return None
    try:
        payload = json.loads(body)
    except json.JSONDecodeError:
        return mask_secret(body[:240].strip())
    message = payload.get("message")
    return mask_secret(str(message)) if message else None


def github_auth_help() -> str:
    return (
        "Run `gh auth status` and "
        "`gh api 'repos/OWNER/REPO/actions/runs?per_page=1'`, "
        "or persist GH_TOKEN/GITHUB_TOKEN in your shell startup file."
    )


def gh_api_endpoint(url: str) -> str:
    if url.startswith(GITHUB_API_ROOT):
        return url[len(GITHUB_API_ROOT):]
    return url


def gh_api_json(url: str) -> dict | None:
    if not gh_available():
        return None
    endpoint = gh_api_endpoint(url)
    result = subprocess.run(["gh", "api", endpoint], capture_output=True, text=True)
    if result.returncode != 0:
        detail = mask_secret((result.stderr or result.stdout).strip())
        if detail:
            print(f"warning: gh api failed for {endpoint}: {detail}", file=sys.stderr)
        return None
    try:
        return json.loads(result.stdout or "{}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"gh api returned invalid JSON for {endpoint}.") from exc


def parse_repo(value: str) -> str | None:
    value = value.strip()
    if value.startswith("git@github.com:"):
        value = value.removeprefix("git@github.com:")
    elif value.startswith("https://github.com/"):
        value = value.removeprefix("https://github.com/")
    elif value.startswith("http://github.com/"):
        value = value.removeprefix("http://github.com/")
    value = value.removesuffix(".git").strip("/")
    parts = value.split("/")
    if len(parts) >= 2:
        return "/".join(parts[:2])
    return None


def default_repo() -> str:
    remote = run_text(["git", "config", "--get", "remote.origin.url"])
    if remote:
        parsed = parse_repo(remote)
        if parsed:
            return parsed
    return DEFAULT_REPO


def current_branch() -> str | None:
    return run_text(["git", "branch", "--show-current"])


def github_token() -> str | None:
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        return token.strip()
    return run_text(["gh", "auth", "token"])


def api_request(url: str, token: str | None) -> urllib.request.Request:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": USER_AGENT,
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return urllib.request.Request(url, headers=headers)


def read_json(url: str, token: str | None) -> dict:
    gh_payload = gh_api_json(url)
    if gh_payload is not None:
        return gh_payload
    try:
        with urllib.request.urlopen(api_request(url, token)) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        if exc.code in (401, 403):
            detail = github_error_message(exc)
            suffix = f" GitHub said: {detail}." if detail else ""
            raise RuntimeError(f"GitHub API authentication failed.{suffix} {github_auth_help()}") from exc
        raise


class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def copy_url_to_file(url: str, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def download_file(url: str, token: str | None, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    opener = urllib.request.build_opener(NoRedirectHandler)
    try:
        with opener.open(api_request(url, token)) as response, destination.open("wb") as output:
            shutil.copyfileobj(response, output)
    except urllib.error.HTTPError as exc:
        if exc.code in (301, 302, 303, 307, 308):
            location = exc.headers.get("Location")
            if not location:
                raise RuntimeError("Artifact download redirect did not include a Location header.") from exc
            copy_url_to_file(location, destination)
            return
        if exc.code in (401, 403):
            raise RuntimeError("Artifact download needs GitHub authentication. Run `gh auth login` or set GH_TOKEN.") from exc
        raise


def copy_directory_contents(source_dir: Path, destination_dir: Path):
    copied: list[str] = []
    destination_dir.mkdir(parents=True, exist_ok=True)
    for source in source_dir.rglob("*"):
        if source.is_dir():
            continue
        relative = source.relative_to(source_dir)
        target = destination_dir / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        copied.append(relative.as_posix())
    return copied


def extract_downloaded_directory(download_dir: Path, artifact_dir: Path, keep_archives: bool):
    if artifact_dir.exists():
        shutil.rmtree(artifact_dir)
    files = [path for path in download_dir.rglob("*") if path.is_file()]
    zip_files = [path for path in files if path.suffix.lower() == ".zip"]
    if len(files) == 1 and len(zip_files) == 1:
        artifact_dir.mkdir(parents=True, exist_ok=True)
        zip_path = zip_files[0]
        if keep_archives:
            shutil.copy2(zip_path, artifact_dir / zip_path.name)
        extract_zip(zip_path, artifact_dir, strip_single_top_level=True)
        return [zip_path.name]
    return copy_directory_contents(download_dir, artifact_dir)


def download_artifact_with_gh(repo: str, run_id: int, name: str, artifact_dir: Path, keep_archives: bool):
    if not shutil.which("gh"):
        return None
    with tempfile.TemporaryDirectory() as temp_name:
        temp_dir = Path(temp_name)
        command = [
            "gh",
            "run",
            "download",
            str(run_id),
            "--repo",
            repo,
            "--name",
            name,
            "--dir",
            str(temp_dir),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            if detail:
                print(f"warning: gh download failed for {name}: {detail}", file=sys.stderr)
            return None
        return extract_downloaded_directory(temp_dir, artifact_dir, keep_archives)

def make_shell_scripts_executable(root: Path) -> list[str]:
    if os.name == "nt":
        return []
    changed: list[str] = []
    for script in root.rglob("*.sh"):
        if not script.is_file():
            continue
        mode = script.stat().st_mode
        new_mode = mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
        if new_mode == mode:
            continue
        script.chmod(new_mode)
        changed.append(script.relative_to(root).as_posix())
    return changed

def latest_successful_run(repo: str, workflow: str, branch: str | None, event: str | None, token: str | None) -> dict:
    workflow_id = urllib.parse.quote(workflow, safe="")
    params = {"status": "success", "per_page": "20"}
    if branch:
        params["branch"] = branch
    if event:
        params["event"] = event
    query = urllib.parse.urlencode(params)
    url = f"https://api.github.com/repos/{repo}/actions/workflows/{workflow_id}/runs?{query}"
    runs = read_json(url, token).get("workflow_runs", [])
    if not runs:
        hint = f" for branch {branch}" if branch else ""
        raise RuntimeError(f"No successful {workflow} run found{hint}.")
    return runs[0]


def list_artifacts(repo: str, run_id: int, token: str | None) -> list[dict]:
    artifacts: list[dict] = []
    page = 1
    while True:
        url = f"https://api.github.com/repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100&page={page}"
        payload = read_json(url, token)
        batch = payload.get("artifacts", [])
        artifacts.extend(batch)
        if len(batch) < 100:
            return artifacts
        page += 1


def selected_artifacts(artifacts: list[dict], pattern: str, exact_names: list[str] | None) -> list[dict]:
    if exact_names:
        wanted = set(exact_names)
        selected = [item for item in artifacts if item.get("name") in wanted]
        missing = sorted(wanted - {item.get("name") for item in selected})
        if missing:
            raise RuntimeError("Artifacts not found: " + ", ".join(missing))
        return selected
    return [item for item in artifacts if fnmatch.fnmatch(item.get("name", ""), pattern)]


def member_parts(name: str, strip_prefix: str | None) -> tuple[str, ...]:
    clean = name.replace("\\", "/")
    parts = PurePosixPath(clean).parts
    if strip_prefix and parts and parts[0] == strip_prefix:
        parts = parts[1:]
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError(f"unsafe archive path: {name}")
    return parts


def common_top_level(zip_file: zipfile.ZipFile) -> str | None:
    prefixes: set[str] = set()
    for info in zip_file.infolist():
        if info.is_dir():
            continue
        parts = PurePosixPath(info.filename.replace("\\", "/")).parts
        if len(parts) < 2:
            return None
        prefixes.add(parts[0])
    if len(prefixes) == 1:
        return next(iter(prefixes))
    return None


def extract_zip(zip_path: Path, destination: Path, strip_single_top_level: bool = True) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    destination_root = destination.resolve()
    with zipfile.ZipFile(zip_path) as archive:
        strip_prefix = common_top_level(archive) if strip_single_top_level else None
        for info in archive.infolist():
            if info.is_dir():
                continue
            parts = member_parts(info.filename, strip_prefix)
            if not parts:
                continue
            target = destination.joinpath(*parts)
            resolved = target.resolve(strict=False)
            if os.path.commonpath([str(destination_root), str(resolved)]) != str(destination_root):
                raise ValueError(f"archive member escapes destination: {info.filename}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open("wb") as output:
                shutil.copyfileobj(source, output)


def extract_artifact_archive(archive_path: Path, artifact_dir: Path, keep_archives: bool) -> list[str]:
    if artifact_dir.exists():
        shutil.rmtree(artifact_dir)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    extracted: list[str] = []
    with tempfile.TemporaryDirectory() as temp_name:
        temp_dir = Path(temp_name)
        with zipfile.ZipFile(archive_path) as outer:
            inner_zips = [info for info in outer.infolist() if not info.is_dir() and info.filename.lower().endswith(".zip")]
            if len(inner_zips) == 1:
                inner_name = Path(inner_zips[0].filename).name
                inner_path = temp_dir / inner_name
                with outer.open(inner_zips[0]) as source, inner_path.open("wb") as output:
                    shutil.copyfileobj(source, output)
                if keep_archives:
                    shutil.copy2(inner_path, artifact_dir / inner_name)
                extract_zip(inner_path, artifact_dir, strip_single_top_level=True)
                extracted.append(inner_name)
            else:
                extract_zip(archive_path, artifact_dir, strip_single_top_level=False)
                extracted.extend(info.filename for info in outer.infolist() if not info.is_dir())
    return extracted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=default_repo(), help="GitHub repository in owner/name form.")
    parser.add_argument("--workflow", default=DEFAULT_WORKFLOW, help="Workflow file name or workflow id.")
    parser.add_argument("--run-id", type=int, help="GitHub Actions run id. If omitted, the latest successful run is used.")
    parser.add_argument("--branch", default=current_branch(), help="Branch used when finding the latest successful run.")
    parser.add_argument("--event", help="Optional event filter, such as push or pull_request.")
    parser.add_argument("--pattern", default=DEFAULT_PATTERN, help="Artifact name glob used when --artifact is not set.")
    parser.add_argument("--artifact", action="append", help="Exact artifact name to download. May be repeated.")
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR.as_posix(), help="Directory for extracted artifacts.")
    parser.add_argument("--keep-archives", action="store_true", help="Keep downloaded outer and inner zip files.")
    parser.add_argument("--clean", action="store_true", help="Remove the run output directory before downloading.")
    args = parser.parse_args()

    token = github_token()
    try:
        if args.run_id:
            run_id = args.run_id
            run_url = f"https://github.com/{args.repo}/actions/runs/{run_id}"
        else:
            run = latest_successful_run(args.repo, args.workflow, args.branch, args.event, token)
            run_id = int(run["id"])
            run_url = run["html_url"]

        output_root = Path(args.output_dir) / f"run-{run_id}"
        if args.clean and output_root.exists():
            shutil.rmtree(output_root)
        output_root.mkdir(parents=True, exist_ok=True)

        artifacts = selected_artifacts(list_artifacts(args.repo, run_id, token), args.pattern, args.artifact)
        artifacts = [item for item in artifacts if not item.get("expired")]
        if not artifacts:
            raise RuntimeError(f"No non-expired artifacts matched {args.pattern!r} in run {run_id}.")

        archive_dir = output_root / "_archives"
        summary = {"repo": args.repo, "run_id": run_id, "run_url": run_url, "artifacts": []}
        for artifact in artifacts:
            name = artifact["name"]
            artifact_dir = output_root / slugify(name)
            outer_zip = archive_dir / f"{slugify(name)}.zip"
            print(f"Downloading {name}...")
            extracted = download_artifact_with_gh(args.repo, run_id, name, artifact_dir, args.keep_archives)
            if extracted is None:
                download_file(artifact["archive_download_url"], token, outer_zip)
                extracted = extract_artifact_archive(outer_zip, artifact_dir, args.keep_archives)
                if not args.keep_archives:
                    outer_zip.unlink(missing_ok=True)
            executable_scripts = make_shell_scripts_executable(artifact_dir)
            if executable_scripts:
                print(f"Marked {len(executable_scripts)} shell scripts executable.")
            summary["artifacts"].append({
                "name": name,
                "path": artifact_dir.as_posix(),
                "files": extracted,
                "executable_scripts": executable_scripts,
            })

        if not args.keep_archives and archive_dir.exists() and not any(archive_dir.iterdir()):
            archive_dir.rmdir()
        (output_root / "artifacts.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(f"Downloaded {len(artifacts)} artifacts to {output_root.as_posix()}")
        print(run_url)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

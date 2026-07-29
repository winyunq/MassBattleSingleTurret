import os
import subprocess
import sys

def main():
    plugin_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(plugin_dir)
    print(f"Working directory: {plugin_dir}")

    # Check git
    if not os.path.exists(".git"):
        print("[1/4] Initializing git repository...")
        subprocess.run(["git", "init"], check=True)
    else:
        print("[1/4] Git repo already exists.")

    # Add & commit
    print("[2/4] Adding files and committing...")
    subprocess.run(["git", "add", "."], check=True)
    res = subprocess.run(["git", "commit", "-m", "docs: update README, README_ZH and add MIT License"], capture_output=True, text=True)
    print(res.stdout or res.stderr or "Committed successfully.")

    # Push via gh CLI or git
    print("[3/4] Checking GitHub repository status...")
    remote_res = subprocess.run(["git", "remote", "get-url", "origin"], capture_output=True, text=True)
    if remote_res.returncode != 0:
        print("Creating GitHub repo MassBattleSingleTurret using gh CLI...")
        gh_res = subprocess.run(["gh", "repo", "create", "MassBattleSingleTurret", "--public", "--source=.", "--remote=origin", "--push"], capture_output=True, text=True)
        print(gh_res.stdout)
        print(gh_res.stderr)
    else:
        print(f"Remote origin exists: {remote_res.stdout.strip()}. Pushing...")
        push_res = subprocess.run(["git", "push", "-u", "origin", "main"], capture_output=True, text=True)
        print(push_res.stdout)
        print(push_res.stderr)

    print("[4/4] Done!")

if __name__ == "__main__":
    main()

# Matt's Git & GitHub Crash Course

This is a beginner-friendly guide to how we are going to use **Git**, **GitHub**, and **VS Code** for this project.

You do **not** need to memorize every command in this guide. The main goal is to understand the workflow and know what to do when you want to make a change.

---

## The Two Most Important Rules

> [!IMPORTANT]
>
> ### 1. Never make changes directly on `main`
>
> `main` is the shared, stable version of the project. **Always create your own branch before making or committing changes.**
>
> Every change should follow this path:
>
> **Create a branch → Make changes → Commit → Push → Open a Pull Request → Review → Merge into `main`**
>
> This repo shouldn't let you push directly to `main`, so always create a branch first.

> [!WARNING]
>
> ### 2. Do not use GitHub's web editor or upload files directly through GitHub
>
> Do **not** use the pencil/Edit button on GitHub to change project files, and do **not** use **Add file → Upload files** to upload your work.
>
> Instead, make changes on your computer using either:
>
> - the `git` command line, or
> - VS Code's built-in Git integration.
>
> The GitHub website is still fine for things like **viewing the repository, opening Pull Requests, reviewing Pull Requests, and discussing changes**. We just do not want to use the website as our code editor.
>
> Working locally makes it easier to review exactly what changed, test your work, keep your local copy synchronized, and make sure every change goes through the same Pull Request process.

> [!TIP]
> If you need any help, ping @matt on Slack!

---

## What Are Git and GitHub?

**Git** is a version-control system. It keeps track of changes to files over time. Think of it like a very powerful version history for a project.

**GitHub** is the website where our Git repository is stored and shared with the rest of the team.

They work together, but they are not the same thing:

- **Git** runs on your computer.
- **GitHub** stores a remote copy of the repository online.

When you edit files, you normally edit them **locally on your computer first** and then use Git to send those changes to GitHub.

---

## A Few Git Words You Should Know

| Term                  | Meaning                                                                      |
| --------------------- | ---------------------------------------------------------------------------- |
| **Repository / Repo** | The project and its Git history.                                             |
| **Clone**             | Download a copy of the repository from GitHub to your computer.              |
| **Branch**            | Your own separate workspace for making a change without changing `main`.     |
| **Stage**             | Choose which changed files will be included in your next commit.             |
| **Commit**            | Save a snapshot of your changes to Git history.                              |
| **Push**              | Send your local commits to GitHub.                                           |
| **Pull**              | Download new commits from GitHub to your computer.                           |
| **Pull Request / PR** | A request to review your branch and merge it into `main`.                    |
| **Merge**             | Combine the approved changes from a branch into `main`.                      |
| **`main`**            | The shared primary branch. Do not directly develop or commit on this branch. |

A simple way to remember the workflow is:

```text
Edit locally
    ↓
Stage changes
    ↓
Commit to your branch
    ↓
Push your branch to GitHub
    ↓
Open a Pull Request
    ↓
Review
    ↓
Merge into main
```

---

# Initial Setup

You only need to do most of this section once per computer.

## 1. Install Git and VS Code

Install:

- Git: https://git-scm.com/
- VS Code: https://code.visualstudio.com/

You can check that Git is installed by opening a terminal and running:

```bash
git --version
```

If Git prints a version number, you are ready to go.

---

## 2. Clone the Repository

Cloning creates a local copy of the GitHub repository on your computer.

Open a terminal and run:

```bash
git clone https://github.com/mmattbtw/disquisition
```

Then enter the new project folder:

```bash
cd disquisition
```

You only need to clone the repository once. After that, you keep using the same local folder.

---

## 3. Set Up Your Git Name and Email

Git records who created each commit.

Set your name:

```bash
git config --global user.name "Your Name"
```

Set the email associated with your GitHub account:

```bash
git config --global user.email "your.github.email@example.com"
```

You can check your settings with:

```bash
git config --global user.name
git config --global user.email
```

---

# The Normal Workflow

This is the process you should follow whenever you work on something for the project.

## 1. Start From an Updated `main`

Before beginning a new task, switch to `main`:

```bash
git switch main
```

Then download the newest changes:

```bash
git pull origin main
```

This helps make sure your new branch starts from the newest version of the project.

> [!IMPORTANT]
> Do not start editing files yet. Create your branch first.

---

## 2. Create Your Own Branch

Branches let everyone work independently without directly changing `main`.

Use this naming format:

```text
<your-name>/<short-description>
```

For example:

```bash
git switch -c matt/add-text-chat
```

This command also works:

```bash
git checkout -b matt/add-text-chat
```

### Make sure you are NOT on `main`

Before making a change, check your current branch:

```bash
git branch --show-current
```

You should see your branch name, such as:

```text
matt/add-text-chat
```

If it says `main`, **do not commit anything yet**. Create or switch to your branch first.

---

## 3. Make Your Changes

Now edit the project files normally in VS Code or another local editor.

When you are finished, save the files and check what Git sees:

```bash
git status
```

`git status` is one of the most useful Git commands. Run it whenever you are unsure what is happening.

To see the actual line-by-line changes you made:

```bash
git diff
```

Always take a quick look at your changes before committing them.

---

## 4. Stage Your Changes

Staging tells Git which files you want included in your next commit.

To stage one specific file:

```bash
git add README.md
```

To stage all current changes:

```bash
git add .
```

Then check again:

```bash
git status
```

Files under **Changes to be committed** are staged and will be included in the next commit.

---

## 5. Commit Your Changes

A commit saves a snapshot of the staged changes to your branch's Git history.

Use a short message describing what you changed:

```bash
git commit -m "Add text chat"
```

Good commit messages are short and specific. Examples:

```text
Add group member to README
Fix login button alignment
Add text chat component
Update project documentation
```

After committing, you can run:

```bash
git status
```

If everything was committed, Git should say your working tree is clean.

---

## 6. Push Your Branch to GitHub

A commit initially exists only on your computer. **Pushing** sends the branch and its commits to GitHub.

The first time you push a new branch, use:

```bash
git push -u origin <your-branch>
```

For example:

```bash
git push -u origin matt/add-text-chat
```

The `-u` connects your local branch with the branch on GitHub. After that, future pushes from the same branch can usually just use:

```bash
git push
```

---

## 7. Open a Pull Request

> [!IMPORTANT]
> **Every change to this project should go through a Pull Request. Never push or commit project work directly to `main`.**

A Pull Request says:

> "Here are the changes on my branch. Please review them before they become part of `main`."

After pushing your branch, go to:

https://github.com/mmattbtw/disquisition

GitHub will usually show a **Compare & pull request** button for the branch you just pushed.

![GitHub Compare & pull request banner](https://docs.github.com/assets/cb-34097/images/help/pull_requests/pull-request-compare-pull-request.png)

Make sure:

```text
base: main
compare: your-name/your-branch
```

Then:

1. Give the Pull Request a useful title.
2. Briefly explain what you changed.
3. Click **Create pull request**.
4. Wait for the Pull Request to be reviewed and merged.

### Optional: Create the PR from the GitHub CLI

If you have the GitHub CLI (`gh`) installed, you can also create a PR without using the website:

```bash
gh pr create --base main --head <your-branch>
```

---

## 8. After Your Pull Request Is Merged

Once your PR has been merged, update your local copy of `main`:

```bash
git switch main
git pull origin main
```

You can then delete your old local branch:

```bash
git branch -d <your-branch>
```

Then, when you start your next task, create a **new branch from the updated `main`**.

---

# VS Code Git Guide

VS Code has Git support built in, so you do not have to use terminal commands for every Git operation.

VS Code is still using the same Git repository underneath. You can freely use the VS Code interface for one step and the Git CLI for another.

## Clone the Repository in VS Code

1. Open VS Code.
2. Click the **Source Control** icon on the left sidebar.
3. Click **Clone Repository**.
4. Paste:

```text
https://github.com/mmattbtw/disquisition.git
```

5. Choose where to save the repository.
6. Open the cloned folder when VS Code asks.

![VS Code Clone Repository](https://code.visualstudio.com/assets/docs/sourcecontrol/quickstart/clone-repository-url.png)

---

## Create a Branch in VS Code BEFORE Editing

This is the most important VS Code step.

Look at the branch name in the bottom-left corner of VS Code. If it says `main`, create a branch **before making or committing changes**.

You can create one by:

1. Clicking the current branch name in the bottom-left status bar.
2. Choosing **Create new branch**.
3. Entering a name such as:

```text
matt/update-readme
```

You can also open the Command Palette and run:

```text
Git: Create Branch...
```

Afterward, confirm the bottom-left corner shows your new branch and **not `main`**.

---

## Make and Review Changes in VS Code

Edit and save your files normally.

Then click the **Source Control** icon on the left. Files you changed will appear under **Changes**.

![VS Code Source Control changed files](https://code.visualstudio.com/assets/docs/sourcecontrol/quickstart/git-modified-files.png)

Click a changed file to open VS Code's diff viewer. This shows the old version and your new version side-by-side so you can check exactly what you changed.

![VS Code diff editor](https://code.visualstudio.com/assets/docs/sourcecontrol/quickstart/diff-editor.png)

Reviewing the diff before committing is a very good habit.

---

## Stage Changes in VS Code

When you are happy with a changed file, hover over it and click the **+** button.

That moves the file from **Changes** to **Staged Changes**.

![VS Code stage changes button](https://code.visualstudio.com/assets/docs/sourcecontrol/quickstart/stage-changes-button.png)

Only staged files are included in your commit.

---

## Commit in VS Code

At the top of the Source Control panel, enter a short commit message, such as:

```text
Add my name to README
```

Then click **Commit**.

![VS Code commit button](https://code.visualstudio.com/assets/docs/sourcecontrol/quickstart/commit-button.png)

Before clicking Commit, take one more look at the branch name. **Do not commit if it says `main`.**

---

## Push Your Branch in VS Code

After committing, VS Code may show **Publish Branch**, **Push**, or **Sync Changes**.

Use that button to send your branch to GitHub.

![VS Code sync changes](https://code.visualstudio.com/assets/docs/sourcecontrol/quickstart/sync-changes.png)

If VS Code asks you to sign into GitHub, follow the sign-in prompt.

Remember: you are pushing **your branch**, not committing directly to `main`.

---

# Guided Tutorial: Add Your Name to `README.md`

For your first practice change, add your name and GitHub account to the project's **Group Members** section.

This exercise intentionally walks through the full workflow so you can practice making a branch and Pull Request.

## Step 1: Update `main`

Open the repository folder in a terminal and run:

```bash
git switch main
git pull origin main
```

---

## Step 2: Create a Branch

Use your own name in the branch name:

```bash
git switch -c your-name/add-name-to-readme
```

Example:

```bash
git switch -c matt/add-name-to-readme
```

Check your branch:

```bash
git branch --show-current
```

**Do not continue if it says `main`.**

---

## Step 3: Edit `README.md`

Open `README.md` in VS Code.

Find the section that looks like this:

```markdown
## Group Members

- Matt Morris [@mmattbtw](https://github.com/mmattbtw)
- Jack Stefl
- Cameron Sapienza
```

Add or update your line using this format:

```markdown
- Your Name [@your-github-username](https://github.com/your-github-username)
```

For example:

```markdown
- Example Person [@example](https://github.com/example)
```

Save the file.

---

## Step 4: Review the Change

Run:

```bash
git diff
```

Or click `README.md` in VS Code's Source Control panel to see the visual diff.

Make sure the only change is the line you intended to edit.

---

## Step 5: Stage the File

```bash
git add README.md
```

Check it:

```bash
git status
```

---

## Step 6: Commit the Change

```bash
git commit -m "Add my name to README"
```

---

## Step 7: Push Your Branch

```bash
git push -u origin your-name/add-name-to-readme
```

Example:

```bash
git push -u origin matt/add-name-to-readme
```

---

## Step 8: Open a Pull Request

Go to:

https://github.com/mmattbtw/disquisition

Click **Compare & pull request**.

Check that the PR says something similar to:

```text
base: main  ←  compare: matt/add-name-to-readme
```

A good PR title would be:

```text
Add Matt's GitHub account to README
```

A simple description could be:

```text
Adds my name and GitHub profile to the Group Members section of README.md.
```

Then click **Create pull request**.

That's it. You have now completed the full workflow:

```text
main
 ↓
new branch
 ↓
edit README.md
 ↓
stage
 ↓
commit
 ↓
push branch
 ↓
pull request
 ↓
review
 ↓
merge into main
```

---

# What If I Accidentally Start Editing on `main`?

If you have changed files but **have not committed yet**, do not panic and do not throw away your work.

Create a new branch immediately:

```bash
git switch -c your-name/description-of-change
```

Your uncommitted changes will normally come with you onto the new branch. Then continue staging and committing there.

---

# Commands You Will Use Most Often

```bash
# See what Git thinks is happening
git status

# See your current branch
git branch --show-current

# Switch to main
git switch main

# Get the latest main
git pull origin main

# Create and switch to a new branch
git switch -c your-name/short-description

# Review changes
git diff

# Stage one file
git add README.md

# Stage all current changes
git add .

# Commit staged changes
git commit -m "Describe your change"

# Push a new branch for the first time
git push -u origin your-name/short-description

# Push later commits on the same branch
git push
```

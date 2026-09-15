# Matt's Git Crash Course

Quick guide in how we're going to use Git in this project.

1. Clone the repository
   Cloning the repository copies the repo from GitHub onto your local machine.

```
git clone https://github.com/mmattbtw/disquisition
```

2. Setup your Git User
   Set your Git user name and email.

```
git config --global user.name "Your Name"
git config --global user.email "your.github.email@example.com"
```

3. Create a branch
   Create a new branch for your changes.

```
git checkout -b <your-name>/<short-description>
```

example:

```
git checkout -b matt/add-text-chat
```

4. Make changes
5. Commit your changes

```
git commit -am "Add text chat"
```

6. Push your changes

```
git push origin <your-branch>
```

7. Open a Pull Request
   Go to the GitHub repository and open a Pull Request.
8. Wait for the Pull Request to be reviewed and merged.
9. Pull the latest changes from the main branch.

```
git checkout main
git pull origin main
```

## VSCode

VS Code has built in git integration. You can view their docs here: https://code.visualstudio.com/docs/sourcecontrol/overview

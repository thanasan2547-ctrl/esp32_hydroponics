# Vercel CI/CD Setup Guide

This guide explains how to set up automatic deployments to Vercel using GitHub Actions.

## Architecture

```
Push to main → GitHub Actions → Vercel Production
Push to feature-branch → GitHub Actions → Vercel Preview URL
Pull Request → GitHub Actions → Vercel Preview URL
```

## Required GitHub Secrets

You need to add **3 secrets** to your GitHub repository:

### 1. `VERCEL_TOKEN`

**What it is:** Personal access token for Vercel CLI authentication

**Where to find it:**
1. Go to [vercel.com/account/tokens](https://vercel.com/account/tokens)
2. Click **"Create Token"**
3. Name it: `GitHub Actions - Hydroponics`
4. Scope: **Full Account**
5. Copy the token (you'll only see it once!)

---

### 2. `VERCEL_ORG_ID`

**What it is:** Your Vercel team/organization ID

**Where to find it:**
1. Go to [vercel.com/account](https://vercel.com/account)
2. Look for **"Your ID"** or **"Team ID"** in Settings
3. Copy the ID (format: `team_xxxxxxxxxxxxx` or `user_xxxxxxxxxxxxx`)

**Alternative method:**
```bash
cd frontend
npx vercel link
cat .vercel/project.json
```
Look for `"orgId"` in the JSON output.

---

### 3. `VERCEL_PROJECT_ID`

**What it is:** The unique ID of your Vercel project

**Where to find it:**

**Method 1 - Via Vercel Dashboard:**
1. Go to your project in [vercel.com](https://vercel.com)
2. Click **Settings** → **General**
3. Scroll down to **"Project ID"**
4. Copy the ID (format: `prj_xxxxxxxxxxxxx`)

**Method 2 - Via CLI:**
```bash
cd frontend
npx vercel link  # Follow prompts to link to existing project
cat .vercel/project.json
```
Look for `"projectId"` in the JSON output.

---

## How to Add Secrets to GitHub

1. Go to your GitHub repository
2. Click **Settings** → **Secrets and variables** → **Actions**
3. Click **"New repository secret"**
4. Add each secret:
   - Name: `VERCEL_TOKEN`, Value: `<your-vercel-token>`
   - Name: `VERCEL_ORG_ID`, Value: `<your-org-id>`
   - Name: `VERCEL_PROJECT_ID`, Value: `<your-project-id>`

---

## First-Time Setup

Before the GitHub Action can work, you need to **link your project** once:

```bash
cd frontend
npx vercel link
```

This creates `.vercel/project.json` with your project configuration. **Do NOT commit this file** (it's gitignored).

---

## Deployment Behavior

| Event | Branch | Result |
|-------|--------|--------|
| Push | `main` | Production deployment at `your-project.vercel.app` |
| Push | `feature-xyz` | Preview deployment at `your-project-git-feature-xyz.vercel.app` |
| Pull Request | any → `main` | Preview deployment with comment in PR |

---

## Workflow Triggers

The GitHub Action **only runs** when:
- Files inside `frontend/` are changed
- The workflow file itself (`.github/workflows/deploy.yml`) is changed

Changes to `backend/` or ESP32 firmware **will NOT** trigger frontend deployment.

---

## Vercel Configuration

Your `frontend/vercel.json` is **compatible** with GitHub Actions deployment. No changes needed.

The workflow uses:
- `vercel pull` — Download project settings
- `vercel build` — Build the project
- `vercel deploy --prebuilt --prod` — Deploy to production (main branch)
- `vercel deploy --prebuilt` — Deploy to preview (other branches)

---

## Testing the Setup

1. Add the 3 secrets to GitHub
2. Make a small change to `frontend/public/index.html`
3. Commit and push to a feature branch:
   ```bash
   git checkout -b test-deploy
   git add frontend/public/index.html
   git commit -m "test: trigger deployment"
   git push origin test-deploy
   ```
4. Check the **Actions** tab in GitHub to see the workflow run
5. Vercel will comment on the commit with the preview URL

---

## Troubleshooting

**Error: "Project not found"**
- Make sure `VERCEL_PROJECT_ID` matches your actual project
- Run `npx vercel link` in `frontend/` directory first

**Error: "Invalid token"**
- Regenerate `VERCEL_TOKEN` and update the GitHub secret
- Make sure token has **Full Account** scope

**Deployment not triggering**
- Check that your changes are in `frontend/` directory
- Verify the workflow file exists at `.github/workflows/deploy.yml`
- Check the **Actions** tab for any errors

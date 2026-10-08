# Disquisition website

The desktop download page at https://disquisition.app is a static Astro site
served by the existing `disquisition` Cloudflare Worker.

Run these commands from `src/web`:

```sh
bun install --frozen-lockfile
bun run astro dev --background
```

Use `bun run astro dev stop`, `status`, and `logs` to manage the dev server.

## Deploy with cf

```sh
bun run deploy
```

This builds Astro, packages `dist` as Cloudflare static assets, and runs
`cf deploy --prebuilt`. `cloudflare.config.ts` sets the Worker name and
`disquisition.app` custom domain. No Astro server adapter is needed.

`scripts/package-worker.ts` uses Cloudflare's Build Output Specification helpers
and validates the output before deployment. The CLI and helpers are pinned
because the `cf` CLI and output format are currently in beta. Generated output
lives in `.cloudflare/output` and is ignored by Git.

To validate an upload without changing the deployed Worker:

```sh
bun run build:worker
bunx cf deploy --prebuilt --dry-run
```

## Cloudflare Git builds

The existing Git integration watches `main`. Its production settings are:

- Root directory: `src/web`
- Build command: `bun install --frozen-lockfile && bun run build:worker`
- Deploy command: `bunx cf deploy --prebuilt`

Preview builds use the same root directory, with these commands:

- Build command: `bun install --frozen-lockfile && bun run build:worker --preview`
- Deploy command: `bunx cf previews deploy --prebuilt`

These settings require the deployment configuration in this directory to be
present on the branch being built. Authenticate with `cf auth login` for local
deployments; Cloudflare Git builds use their existing build token.

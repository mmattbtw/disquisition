import { access } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { InputWorkerSchema } from '@cloudflare/config';
import {
	cleanBuildOutputDir,
	readBuildOutput,
	writeAssets,
	writeRootConfig,
	writeWorkerConfig,
} from '@cloudflare/build-output-utils';
import config from '../cloudflare.config';

const root = fileURLToPath(new URL('../', import.meta.url));
const assets = fileURLToPath(new URL('../dist/', import.meta.url));
const context = {
	isPreview: process.argv.includes('--preview'),
	mode: 'production',
};
const resolvedConfig = config(context);

// Package the static Astro output for cf deploy --prebuilt. No server Worker
// or Astro adapter is needed for this page.
await access(new URL('../dist/index.html', import.meta.url));
await cleanBuildOutputDir(root);
await writeRootConfig(root, {}, context);
await writeWorkerConfig({ root, config: InputWorkerSchema.parse(resolvedConfig.worker) });
await writeAssets({ root, sourceDirectory: assets });
await readBuildOutput(root);

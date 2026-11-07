import { appendFileSync, mkdirSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import config from '../cloudflare.config';

// Workers Builds fills in the PR comment's preview URLs from the
// WRANGLER_OUTPUT_FILE_* entry that `wrangler preview` writes. `cf previews
// deploy` prints the same entry to stdout but never writes the file, so the
// comment always says "No Preview URL". Write it ourselves until cf does.

const cf = fileURLToPath(new URL('../node_modules/.bin/cf', import.meta.url));
const proc = Bun.spawn([cf, 'previews', 'deploy', '--prebuilt', ...process.argv.slice(2)], {
	stdout: 'pipe',
	stderr: 'inherit',
	stdin: 'inherit',
});
const stdout = await new Response(proc.stdout).text();
process.stdout.write(stdout);
const exitCode = await proc.exited;
if (exitCode !== 0) process.exit(exitCode);

const outputFile = getOutputFilePath();
if (outputFile) {
	const start = stdout.indexOf('{');
	const end = stdout.lastIndexOf('}');
	const result = start === -1 ? undefined : JSON.parse(stdout.slice(start, end + 1));
	if (result?.type !== 'preview') {
		console.error('cf previews deploy did not print a preview result; skipping output file');
		process.exit(1);
	}
	const workerName = config({ isPreview: true, mode: 'production' }).worker.name;
	const entry = { ...result, worker_name: workerName, timestamp: new Date().toISOString() };
	mkdirSync(dirname(outputFile), { recursive: true });
	appendFileSync(outputFile, `${JSON.stringify(entry)}\n`);
}

// Same resolution as wrangler's getOutputFilePath.
function getOutputFilePath() {
	if (process.env.WRANGLER_OUTPUT_FILE_PATH) return process.env.WRANGLER_OUTPUT_FILE_PATH;
	const directory = process.env.WRANGLER_OUTPUT_FILE_DIRECTORY;
	if (!directory) return null;
	const date = new Date().toISOString().replaceAll(':', '-').replace('.', '_').replace('T', '_').replace('Z', '');
	return resolve(directory, `wrangler-output-${date}-${randomBytes(3).toString('hex')}.json`);
}

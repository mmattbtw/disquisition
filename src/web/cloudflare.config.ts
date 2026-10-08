import { defineConfig } from "cf/config";

export default defineConfig(({ isPreview }) => ({
	worker: {
		name: "disquisition",
		compatibilityDate: "2026-10-08",
		domains: isPreview ? undefined : ["disquisition.app"],
		assets: {
			notFoundHandling: "none",
		},
	},
}));

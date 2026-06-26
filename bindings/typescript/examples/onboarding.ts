/**
 * Canonical flowd (TypeScript) host for onboarding.flow — batteries included.
 *
 * The host writes only the real per-tool logic against the generated, typed
 * `Onboard` contract (run `npx flowd-codegen onboarding.ir.json >
 * onboarding.flow.ts` to produce it):
 *
 *   - fetchCredit     deterministic : an external lookup (here in-memory)
 *   - assessRisk      model         : an offline heuristic fallback; when
 *                                     ANTHROPIC_API_KEY is set the model step is
 *                                     served by Claude with *no adapter code* —
 *                                     anthropicProvider derives the structured-
 *                                     output schema from the IR, reports token
 *                                     usage into the trace, and hides the
 *                                     synchronous bridge to the async SDK.
 *   - notifyApplicant mutation      : the side effect (sends an email)
 *
 * secretRedactor keeps credentials out of the trace. The wiring is two lines.
 *
 *   npm install                          # in the binding; builds the addon
 *   npm install @anthropic-ai/sdk        # only for the Claude path
 *   npx flowd-codegen onboarding.ir.json > onboarding.flow.ts
 *   tsc onboarding.ts onboarding.flow.ts --module commonjs --target es2022 \
 *       --moduleResolution node --esModuleInterop
 *   node onboarding.js
 */

import * as path from "node:path";

import { Suspension, secretRedactor, anthropicProvider } from "../dist/index";
import { Onboard, type OnboardTools } from "./onboarding.flow.js";

// A toy credit bureau. fetchCredit stays deterministic (level 1): a real impl
// would HTTP GET a bureau, with no side effects.
const BUREAU: Record<string, { score: number; history_years: number }> = {
  cust_42: { score: 760, history_years: 9 },
  cust_99: { score: 590, history_years: 2 },
};

const impl: OnboardTools = {
  fetchCredit: ({ id }) => BUREAU[id] ?? { score: 600, history_years: 0 },

  // Offline fallback; Claude serves this when a provider is registered.
  assessRisk: ({ report, amount }) => {
    const band = report.score >= 720 ? "LOW" : report.score >= 640 ? "MEDIUM" : "HIGH";
    return {
      approve: report.score >= 640 && amount <= report.score * 40,
      band,
      rationale: `heuristic: score ${report.score}, requested ${amount}`,
    };
  },

  // Mutation: a real impl sends email via SES/SMTP with an idempotency key —
  // replay restores level-3 nodes and never re-invokes.
  notifyApplicant: ({ email, band }) => {
    console.log(`  [email] -> ${email}: your application risk band is ${band}`);
    return true;
  },
};

function main(): void {
  const traceDir = path.join(__dirname, "traces");
  const irPath = path.join(__dirname, "onboarding.ir.json");
  const onboard = new Onboard(impl, { traceDir, implVersion: "onboarding-host-1" });
  try {
    onboard.setRedactor(secretRedactor()); // scrub any key-shaped bytes

    if (process.env.ANTHROPIC_API_KEY) {
      onboard.registerProvider(
        anthropicProvider(irPath, {
          instructions: {
            "claude-underwriter-v1":
              "You are a conservative consumer-credit underwriter. Decide whether " +
              "to approve, assign a LOW/MEDIUM/HIGH band, and give a one-sentence rationale.",
          },
        }),
      );
      console.log("underwriting via Claude (claude-opus-4-8)");
    } else {
      console.log("underwriting via heuristic (set ANTHROPIC_API_KEY for Claude)");
    }

    const result = onboard.run({ id: "cust_42", email: "alice@example.com", amount: 15000 });

    if (result instanceof Suspension) {
      console.log(`suspended: ${result.token}`); // this flow never suspends
      return;
    }
    console.log("decision:", JSON.stringify(result));
    console.log(`trace written under ${traceDir}/onboard/`);
  } finally {
    onboard.close();
  }
}

main();

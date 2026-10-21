#pragma once
// Folder names shared by qpm, which installs packages into them, and the
// Quantum compiler, which finds packages there for `import` and `# @use`.

namespace qpm
{

    // Quantum packages from the QPM registry: <project>/quantum_qubits/<name>.
    inline constexpr const char *kQubitsDir = "quantum_qubits";

    // npm packages (installs fall back to npm for names the QPM registry doesn't
    // have): <project>/node_modules/<name>, the only place Node.js looks for them.
    inline constexpr const char *kNodeModulesDir = "node_modules";

} // namespace qpm

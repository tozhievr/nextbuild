'use strict';

const path = require('node:path');
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;
let pending = Promise.resolve();

function executable(uri) {
    const setting = vscode.workspace.getConfiguration('nextbuild', uri).get('executablePath');
    return setting?.trim() || 'nextbuild';
}

function activeFolder() {
    const uri = vscode.window.activeTextEditor?.document.uri;
    const folders = vscode.workspace.workspaceFolders ?? [];
    return (uri && vscode.workspace.getWorkspaceFolder(uri)) ||
        (folders.length === 1 ? folders[0] : undefined);
}

async function runTask(clean) {
    let folder = activeFolder();
    if (!folder) folder = await vscode.window.showWorkspaceFolderPick();
    if (!folder || folder.uri.scheme !== 'file') return;
    const root = folder.uri.fsPath;
    const args = ['--root', root];
    if (clean) args.push('--clean');
    const uri = vscode.window.activeTextEditor?.document.uri;
    if (uri?.scheme === 'file' && vscode.workspace.getWorkspaceFolder(uri)?.uri.toString() === folder.uri.toString() &&
        path.basename(uri.fsPath).endsWith('.nextbuild')) {
        args.push(uri.fsPath);
    }
    const label = clean ? 'Clean' : 'Build';
    const task = new vscode.Task({ type: 'nextbuild', command: label.toLowerCase() }, folder, label,
        'nextbuild', new vscode.ProcessExecution(executable(folder.uri), args, { cwd: root }), ['$gcc']);
    if (!clean) task.group = vscode.TaskGroup.Build;
    await vscode.tasks.executeTask(task);
}

function activate(context) {
    const output = vscode.window.createOutputChannel('Nextbuild');
    const restart = () => {
        pending = pending.catch(() => {}).then(async () => {
            if (client) await client.dispose();
            client = undefined;
            if (!vscode.workspace.isTrusted) return;
            const folder = activeFolder() ?? vscode.workspace.workspaceFolders?.[0];
            const command = executable(folder?.uri);
            const server = new LanguageClient('nextbuild', 'Nextbuild',
                { command, args: ['lsp', '--stdio'], transport: TransportKind.stdio,
                  options: { cwd: folder?.uri.scheme === 'file' ? folder.uri.fsPath : undefined } },
                { documentSelector: [{ scheme: 'file', language: 'nextbuild' }], outputChannel: output });
            try {
                await server.start();
                client = server;
            } catch (error) {
                await server.dispose();
                output.appendLine(String(error));
                void vscode.window.showErrorMessage('Nextbuild could not start. Install nextbuild or set nextbuild.executablePath.');
            }
        });
        return pending;
    };
    context.subscriptions.push(output,
        vscode.commands.registerCommand('nextbuild.build', () => runTask(false)),
        vscode.commands.registerCommand('nextbuild.clean', () => runTask(true)),
        vscode.commands.registerCommand('nextbuild.restartLanguageServer', restart),
        vscode.workspace.onDidChangeConfiguration(event => {
            if (event.affectsConfiguration('nextbuild.executablePath')) void restart();
        }));
    return restart();
}

async function deactivate() {
    await pending;
    if (client) await client.dispose();
    client = undefined;
}

module.exports = { activate, deactivate };

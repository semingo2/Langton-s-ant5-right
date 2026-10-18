# GitHubへ公開する手順（簡易版）

## 1. 新しいリポジトリ

GitHubで **New repository** を選び、例 `langtons-ant-cpp` という名前で作成します。公開する場合は Public を選びます。

## 2. ファイルをアップロード

このフォルダーにある `main.cpp`, `index.html`, `style.css`, `app.js`, `Dockerfile`, `render.yaml`, `README.md`, `.gitignore` をリポジトリのルートに追加してコミットします。

## 3. サイトのデプロイ

RenderにサインインしてGitHub連携後、**New → Blueprint** からそのリポジトリを選択し、`render.yaml` を確認してデプロイします。サイトの公開URLはRenderが作成したサービス画面に表示されます。

## なぜGitHub Pagesではないの？

このアプリの計算処理はブラウザー内のJavaScriptではなく、C++で動くHTTPサーバーにあります。GitHub Pagesは静的ファイル配信なので、このサーバーを起動できません。GitHubはソースコードの公開先、RenderはC++サーバーを動かす場所という分担です。


## 利用者ごとの盤面分離

公開版はCookie `langtons_ant_session` でブラウザーセッションを分けます。別々のブラウザー／ブラウザープロファイルで開いた利用者は独立した盤面を使います。同じブラウザープロファイルの複数タブは同じセッションを使います。12時間操作がないセッションは自動削除され、Renderが再起動するとメモリ上のシミュレーションはリセットされます。

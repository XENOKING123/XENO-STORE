-- XENO STORE · Save Data Store · migration 0005
-- Console shares: a console uploads the exported zip into the bucket as
--   xeno/<yyyymmdd>-<16 hex>.zip
-- (the path it uses on the XenoKing site too — cx_savestore.c, upload host
-- "site") and then inserts one row whose file_path is that path. 0004's upload
-- policy only took <PPSA id>/<name>.(zip|png), so a deployment of this
-- template refused every console upload. This policy takes both shapes; rows
-- still arrive as status = 'pending' (0004's insert policy is unchanged).
-- Run it after 0004. Idempotent: safe to run twice.

DROP POLICY IF EXISTS "Consoles upload ps5-saves" ON storage.objects;
CREATE POLICY "Consoles upload ps5-saves" ON storage.objects
  FOR INSERT TO anon, authenticated
  WITH CHECK (bucket_id = 'ps5-saves' AND (name ~ '^PPSA[0-9]{5}/[A-Za-z0-9._-]{1,200}\.(zip|png)$' OR name ~ '^xeno/[0-9]{8}-[0-9a-f]{16}\.zip$'));

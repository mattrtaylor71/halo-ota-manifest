"""One S3 transmission per release operation using the installed CLI event API.

Credential bootstrap is separate. Headers, signatures and credentials never enter
the audit. A redirect/retry reaches before-send again and is refused before I/O.
"""
import configparser
import json
import os
from pathlib import Path
import sys
import time
from urllib.parse import parse_qs, unquote, urlparse

SCOPES = {'halo-ota-prod': 'halo/ota/prod/',
          'halo-ota-dev': 'halo/ota/canary/production-release-20260909/dev/'}


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


class OneS3Send:
    def __init__(self, args, audit):
        self.audit, self.seen, self.allowed = Path(audit), 0, 0
        def one(flag):
            require(args.count(flag) == 1, 'Exactly one ' + flag + ' required')
            return args[args.index(flag) + 1]
        self.bucket = one('--bucket')
        require(self.bucket in SCOPES, 'Release bucket refused')
        require(one('--region') == 'us-east-1' and '--endpoint-url' not in args, 'Release region/endpoint refused')
        self.operation = one('s3api')
        require(self.operation in ('put-object', 'list-objects-v2'), 'Release operation refused')
        self.key = one('--key') if self.operation == 'put-object' else None
        self.prefix = one('--prefix') if self.key is None else None
        target = self.key if self.key is not None else self.prefix
        require(target.startswith(SCOPES[self.bucket]) and '..' not in target, 'Out-of-scope release key')
        self.condition = None
        if self.key is not None:
            require(('--if-match' in args) != ('--if-none-match' in args), 'Exactly one conditional write required')
            flag = '--if-match' if '--if-match' in args else '--if-none-match'
            self.condition = (flag[2:], one(flag))
            require(self.condition[1] == '*' if flag == '--if-none-match' else
                    self.key.endswith('/manifest_latest.json') and bool(self.condition[1]), 'Invalid write condition')
        require(not self.audit.exists(), 'Fresh transmission audit required')

    def __call__(self, request, **kwargs):
        self.seen += 1
        url = urlparse(request.url)
        good = (self.seen == 1 and url.scheme == 'https' and
                url.hostname == self.bucket + '.s3.us-east-1.amazonaws.com' and
                not url.port and not url.username and not url.password)
        if self.key is not None:
            headers = {(k.decode() if isinstance(k, bytes) else k).lower():
                       (v.decode() if isinstance(v, bytes) else v) for k, v in request.headers.items()}
            good = good and request.method == 'PUT' and unquote(url.path) == '/' + self.key
            good = good and not url.query and headers.get(self.condition[0]) == self.condition[1]
        else:
            query = parse_qs(url.query)
            good = good and request.method == 'GET' and url.path == '/' and query.get('prefix') == [self.prefix] and query.get('list-type') == ['2']
        row = {'epoch': time.time(), 's3_before_send_number': self.seen, 'allowed': bool(good),
               'operation': self.operation, 'method': request.method, 'url_host': url.hostname,
               'key': self.key, 'prefix': self.prefix, 'credential_bootstrap_not_counted': True}
        with self.audit.open('a') as f:
            json.dump(row, f); f.write('\n'); f.flush(); os.fsync(f.fileno())
        require(good, 'S3 transmission/endpoint/condition guard refused request; no resend')
        self.allowed += 1


def driver(args, audit):
    require(os.environ.get('AWS_MAX_ATTEMPTS') == '1' and os.environ.get('AWS_RETRY_MODE') == 'standard' and
            os.environ.get('AWS_IGNORE_CONFIGURED_ENDPOINT_URLS') == 'true', 'Release environment differs')
    config = Path(os.environ.get('AWS_CONFIG_FILE', str(Path.home() / '.aws/config')))
    parser = configparser.RawConfigParser(); parser.read(str(config))
    require(not (parser.has_section('plugins') and parser.items('plugins')), 'Configured CLI plugins refused')
    import awscli.clidriver
    guard = OneS3Send(args, audit)
    cli = awscli.clidriver.create_clidriver(args)
    cli.session.register('before-send.s3', guard, unique_id='halo-release-one-s3-transmission')
    code = cli.main(args)
    require(guard.allowed == 1, 'CLI did not complete its one permitted S3 transmission')
    return code


if __name__ == '__main__':
    raise SystemExit(driver(sys.argv[2:], sys.argv[1]))

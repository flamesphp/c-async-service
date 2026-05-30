# cflames_async_service

Real multi-process parallelism for PHP closures — implemented as a native C extension.

## Why not Fibers?

PHP Fibers (8.1+) are cooperative coroutines: only one fiber runs at a time, and the scheduler must explicitly yield control. There is no true CPU-level parallelism — two fibers calling `sleep(1)` still take **2 seconds** total, not 1.

This extension uses `fork(2)` to create actual OS child processes. Each `Service::async(...)` spawns a real process that runs concurrently with the parent **and** with every other async task. Two tasks sleeping for 1 and 2 seconds complete in **~2 seconds** total, not 3.

No ZTS (thread-safe PHP) is required — `fork` works with any standard PHP build.

---

## Requirements

- PHP 8.0 or newer (Linux / macOS)
- GCC or Clang
- `phpize`, `autoconf`, `make`
- **Optional**: [igbinary](https://github.com/igbinary/igbinary) extension for faster binary serialization

---

## Installation

```bash
phpize
./configure --enable-cflames-async-service
make -j$(nproc)
sudo make install
```

Add to `php.ini`:

```ini
extension=cflames_async_service.so
```

Or use the provided `Dockerfile` for a ready-to-run Docker image.

---

## API

### `\Flames\Async\Service::async(callable $fn): \Flames\Async\Service\Task`

Forks immediately and begins executing `$fn()` in a child process.  
**Returns instantly** — does not wait for the child to finish.

The closure's `use (...)` variables are part of the process image after `fork`, so they are captured naturally.

```php
use Flames\Async\Service;

$task = Service::async(function() use ($someVar) {
    // runs in a separate OS process, in true parallel
    return $someVar . '_processed';
});
```

### `\Flames\Async\Service::await(\Flames\Async\Service\Task ...$tasks): mixed`

Blocks until all listed tasks have finished, then returns their results.

| Call | Return value |
|---|---|
| `Service::await($task)` | The value returned by `$fn` directly |
| `Service::await($t1, $t2, …)` | Indexed array `[result0, result1, …]` |

```php
$result  = Service::await($task);
$results = Service::await($task1, $task2);  // [0 => ..., 1 => ...]
```

### `\Flames\Async\Service\Task::isDone(): bool`

Non-blocking poll. Returns `true` if the child has already finished.

### `\Flames\Async\Service\Task::result(): mixed`

Blocks until done and returns the value. Equivalent to `Service::await($this)`.

---

## Examples

### Basic parallel execution

```php
use Flames\Async\Service;

$type = 'non-admin';

$getUsers = Service::async(function() use ($type) {
    sleep(2); // simulate DB query
    return ['users' => [], 'type' => $type];
});

$getConfigs = Service::async(function() {
    sleep(1); // simulate DB query
    return ['debug' => false, 'version' => '2.0'];
});

// Both children are already running concurrently.
// This takes ~2 s (max), not 3 s (sum).
[$users, $configs] = Service::await($getUsers, $getConfigs);

var_dump($users);   // ['users' => [], 'type' => 'non-admin']
var_dump($configs); // ['debug' => false, 'version' => '2.0']
```

### Awaiting a single task

```php
use Flames\Async\Service;

$task = Service::async(function() {
    return array_sum(range(1, 1_000_000));
});

$sum = Service::await($task);
echo $sum; // 500000500000
```

### Exception propagation

If the closure throws, the original message is re-thrown in the parent at `await()` / `result()` time.

```php
use Flames\Async\Service;

$task = Service::async(function() {
    throw new \RuntimeException('Something went wrong');
});

try {
    Service::await($task);
} catch (\RuntimeException $e) {
    echo $e->getMessage(); // "Something went wrong"
}
```

### Polling without blocking

```php
use Flames\Async\Service;

$task = Service::async(function() {
    sleep(3);
    return 'done';
});

while (!$task->isDone()) {
    echo "Still running…\n";
    sleep(1);
}

echo Service::await($task); // "done"
```

---

## Serialization

The child's return value travels through a pipe as a serialized byte stream.

| Condition | Serializer used |
|---|---|
| igbinary extension loaded | `igbinary_serialize` / `igbinary_unserialize` |
| igbinary not available | PHP native `serialize` / `unserialize` |

The check is done once per process and cached. `phpinfo()` shows the active serializer under the **Flames Async Service** section.

> **Note:** Return values must be serializable. Closures, resources, and objects that implement `__serialize` incorrectly cannot be returned from an async task.

---

## How it works internally

```
Service::async(fn)
│
├─ pipe(fds)           → parent reads fd[0], child writes fd[1]
├─ fork()
│   ├─ CHILD ───────── call_user_function(fn, &retval)
│   │                  flames_serialize(retval)       ← igbinary or native
│   │                  write(fd[1], 0x01 + payload)
│   │                  _exit(0)    ← bypasses PHP/C++ atexit handlers
│   │
│   └─ PARENT ──────── close(fd[1])
│                      store fd[0] + pid in Task object
│                      return Task
│
Service::await($t1, $t2)
│
├─ read(fd[0] of $t1)  → blocks until child closes its write end
├─ waitpid(pid of $t1) → reap (no zombie)
├─ flames_unserialize  → zval
├─ read(fd[0] of $t2)  → …
└─ return [result0, result1]
```

Wire protocol (1-byte header):

| Byte | Payload |
|---|---|
| `0x01` | Serialized return value (igbinary or PHP native) |
| `0x02` | UTF-8 exception message |

---

## Limitations

- **Linux / macOS only** — `fork()` is POSIX; not available on Windows.
- **Serializable return values** — the return value crosses a process boundary via the pipe.
- **No shared mutable state** — child processes have independent memory (CoW). Mutations inside the closure are invisible to the parent. This prevents data races by design.
- **DB connections** — if your DB driver does not handle post-fork reconnection automatically, reconnect at the top of the closure.

---

## Building with Docker

```bash
docker build -t flames-async .

docker run --rm flames-async php -r "
    use Flames\Async\Service;
    \$a = Service::async(function(){ sleep(1); return 42; });
    \$b = Service::async(function(){ sleep(2); return 'ok'; });
    \$r = Service::await(\$a, \$b);
    echo \$r[0] . ' ' . \$r[1] . PHP_EOL;
"
# Output: 42 ok   (printed after ~2 s, not 3 s)
```

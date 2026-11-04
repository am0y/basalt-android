package com.roblox.client.startup;

import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Locale;
import java.util.Iterator;
import java.util.ArrayDeque;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;
import java.util.regex.Matcher;
import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import javax.crypto.spec.IvParameterSpec;
import okhttp3.Authenticator;
import okhttp3.CookieJar;
import okhttp3.HttpUrl;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;
import org.json.JSONArray;
import org.json.JSONObject;

/** Executor HTTP is deliberately independent of the application's cookie store. */
public final class BasaltHttp {
    private static final int MAX_BODY = 8 * 1024 * 1024;
    private static final OkHttpClient CLIENT = new OkHttpClient.Builder()
        .cookieJar(CookieJar.NO_COOKIES)
        .authenticator(Authenticator.NONE)
        .proxyAuthenticator(Authenticator.NONE)
        .cache(null)
        .followRedirects(false)
        .followSslRedirects(false)
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .callTimeout(20, TimeUnit.SECONDS)
        .build();
    private static final OkHttpClient SOCKET_CLIENT = CLIENT.newBuilder()
        .followRedirects(true).followSslRedirects(true).build();

    private BasaltHttp() {}

    private static JSONObject cryptoOperation(JSONObject options) throws Exception {
        String operation = options.getString("Operation");
        if (operation.equals("random")) {
            double requested = options.getDouble("Count");
            if (!Double.isFinite(requested) || requested != Math.floor(requested) || requested < 0 || requested > 1024 * 1024)
                throw new IOException("invalid random byte count");
            byte[] bytes = new byte[(int)requested];
            new SecureRandom().nextBytes(bytes);
            return new JSONObject().put("data", Base64.encodeToString(bytes, Base64.NO_WRAP));
        }
        byte[] bytes = Base64.decode(options.getString("Data"), Base64.DEFAULT);
        if (bytes.length > MAX_BODY) throw new IOException("crypto input exceeds 8 MiB");
        if (operation.equals("encrypt") || operation.equals("decrypt")) {
            String mode = options.optString("Mode", "CBC").toUpperCase(Locale.ROOT);
            if (!mode.matches("CBC|CTR|CFB|OFB|ECB")) throw new IOException("unsupported AES mode");
            byte[] key = Base64.decode(options.getString("Key"), Base64.DEFAULT);
            if (key.length != 16 && key.length != 24 && key.length != 32) throw new IOException("AES key must contain 16, 24 or 32 bytes");
            byte[] iv = options.has("IV") ? Base64.decode(options.getString("IV"), Base64.DEFAULT) : new byte[16];
            if (!options.has("IV")) new SecureRandom().nextBytes(iv);
            if (iv.length != 16) throw new IOException("AES IV must contain 16 bytes");
            String padding = mode.equals("CBC") || mode.equals("ECB") ? "PKCS5Padding" : "NoPadding";
            Cipher cipher = Cipher.getInstance("AES/" + mode + "/" + padding);
            int direction = operation.equals("encrypt") ? Cipher.ENCRYPT_MODE : Cipher.DECRYPT_MODE;
            SecretKeySpec secret = new SecretKeySpec(key, "AES");
            if (mode.equals("ECB")) cipher.init(direction, secret);
            else cipher.init(direction, secret, new IvParameterSpec(iv));
            return new JSONObject().put("data", Base64.encodeToString(cipher.doFinal(bytes), Base64.NO_WRAP))
                .put("iv", Base64.encodeToString(iv, Base64.NO_WRAP));
        }
        if (!operation.equals("hash") && !operation.equals("hmac")) throw new IOException("unsupported crypto operation");
        String name = options.optString("Algorithm", "sha384").toLowerCase(Locale.ROOT).replace("-", "");
        String algorithm;
        switch (name) {
            case "md5": algorithm = "MD5"; break;
            case "sha1": algorithm = "SHA-1"; break;
            case "sha224": algorithm = "SHA-224"; break;
            case "sha256": algorithm = "SHA-256"; break;
            case "sha384": algorithm = "SHA-384"; break;
            case "sha512": algorithm = "SHA-512"; break;
            default: throw new IOException("unsupported hash algorithm");
        }
        if (operation.equals("hmac")) {
            Mac mac = Mac.getInstance("Hmac" + algorithm.replace("-", ""));
            byte[] key = Base64.decode(options.getString("Key"), Base64.DEFAULT);
            mac.init(new SecretKeySpec(key.length == 0 ? new byte[1] : key, mac.getAlgorithm()));
            return new JSONObject().put("data", Base64.encodeToString(mac.doFinal(bytes), Base64.NO_WRAP));
        }
        byte[] digest = MessageDigest.getInstance(algorithm).digest(bytes);
        char[] text = new char[digest.length * 2], hex = "0123456789abcdef".toCharArray();
        for (int i = 0; i < digest.length; ++i) {
            text[2 * i] = hex[(digest[i] & 255) >>> 4];
            text[2 * i + 1] = hex[digest[i] & 15];
        }
        return new JSONObject().put("digest", new String(text));
    }

    private static JSONObject regexOperation(JSONObject options) throws Exception {
        String expression = options.getString("Pattern");
        String input = options.optString("Input", "");
        if (expression.length() > 4096 || input.length() > 1024 * 1024) throw new IOException("regex input is too large");
        Matcher matcher = Pattern.compile(expression).matcher(input);
        switch (options.getString("Operation")) {
            case "compile": return new JSONObject();
            case "match": return new JSONObject().put("value", matcher.find() ? matcher.group() : JSONObject.NULL);
            case "many": {
                JSONArray matches = new JSONArray();
                while (matcher.find()) {
                    if (matches.length() >= 10000) throw new IOException("too many regex matches");
                    matches.put(matcher.group());
                }
                return new JSONObject().put("values", matches);
            }
            case "replace": return new JSONObject().put("value", matcher.replaceAll(options.getString("Replacement")));
            default: throw new IOException("unsupported regex operation");
        }
    }

    public static byte[] execute(byte[] encoded) throws Exception {
        JSONObject options = new JSONObject(new String(encoded, StandardCharsets.UTF_8));
        if (options.has("BridgeOperation")) {
            if ("crypto".equals(options.getString("BridgeOperation")))
                return cryptoOperation(options).toString().getBytes(StandardCharsets.UTF_8);
            if ("regex".equals(options.getString("BridgeOperation")))
                return regexOperation(options).toString().getBytes(StandardCharsets.UTF_8);
            if (!"websocket".equals(options.getString("BridgeOperation"))) throw new IOException("unsupported operation");
            return socketOperation(options).toString().getBytes(StandardCharsets.UTF_8);
        }
        String url = options.optString("Url", options.optString("URL", ""));
        String method = options.optString("Method", "GET");
        if (!method.matches("GET|POST|PUT|PATCH|DELETE|HEAD")) throw new IOException("unsupported HTTP method");
        HttpUrl parsed = HttpUrl.get(url);
        if (!parsed.username().isEmpty() || !parsed.password().isEmpty()) throw new IOException("URL credentials are not supported");
        boolean binary = options.has("BodyBase64"), plain = options.has("Body");
        if (binary && plain) throw new IOException("Body and BodyBase64 cannot both be set");
        byte[] bytes = binary ? Base64.decode(options.getString("BodyBase64"), Base64.DEFAULT) :
            plain ? options.getString("Body").getBytes(StandardCharsets.UTF_8) : null;
        if (bytes != null && bytes.length > MAX_BODY) throw new IOException("Body exceeds 8 MiB");
        if (bytes == null && (method.equals("POST") || method.equals("PUT") || method.equals("PATCH"))) bytes = new byte[0];
        RequestBody body = bytes == null ? null : RequestBody.create((MediaType)null, bytes);
        Request.Builder builder = new Request.Builder().url(parsed).method(method, body);
        JSONObject headers = options.optJSONObject("Headers");
        boolean userAgent = false;
        if (headers != null) {
            if (headers.length() > 80) throw new IOException("too many HTTP headers");
            Iterator<String> keys = headers.keys();
            while (keys.hasNext()) {
                String name = keys.next();
                Object value = headers.get(name);
                if (!(value instanceof String)) throw new IOException("header values must be strings");
                builder.header(name, (String)value);
                if (name.equalsIgnoreCase("User-Agent")) userAgent = true;
            }
        }
        if (!userAgent) builder.header("User-Agent", "Roblox/WinInet");
        Request request = builder.build();
        int timeout = options.optInt("Timeout", 20);
        if (timeout < 1 || timeout > 120) throw new IOException("Timeout must be between 1 and 120 seconds");
        Object redirectOption = options.has("FollowRedirects") ? options.get("FollowRedirects") : Boolean.TRUE;
        if (!(redirectOption instanceof Boolean)) throw new IOException("FollowRedirects must be a boolean");
        boolean followRedirects = (Boolean)redirectOption;
        OkHttpClient client = options.has("Timeout") ? CLIENT.newBuilder()
            .readTimeout(timeout, TimeUnit.SECONDS).callTimeout(timeout, TimeUnit.SECONDS).build() : CLIENT;
        for (int redirects = 0; redirects <= 10; redirects++) {
            try (Response response = client.newCall(request).execute()) {
                int status = response.code();
                String location = response.header("Location");
                if (followRedirects && location != null &&
                    (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
                    if (redirects == 10) throw new IOException("too many HTTP redirects");
                    HttpUrl target = request.url().resolve(location);
                    if (target == null || !target.username().isEmpty() || !target.password().isEmpty())
                        throw new IOException("invalid HTTP redirect");
                    if (request.url().isHttps() && !target.isHttps()) throw new IOException("HTTPS downgrade redirect blocked");
                    Request.Builder next = request.newBuilder().url(target);
                    if (!request.url().host().equals(target.host()) || request.url().port() != target.port() ||
                        !request.url().scheme().equals(target.scheme())) {
                        next.removeHeader("Authorization").removeHeader("Proxy-Authorization")
                            .removeHeader("Cookie").removeHeader("Cookie2").removeHeader("Host");
                    }
                    if ((status == 303 && !request.method().equals("HEAD")) ||
                        ((status == 301 || status == 302) && request.method().equals("POST"))) {
                        next.method("GET", null).removeHeader("Content-Type").removeHeader("Content-Length");
                    }
                    request = next.build();
                    continue;
                }
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                if (response.body() != null) {
                    try (InputStream input = response.body().byteStream()) {
                        byte[] chunk = new byte[65536];
                        for (int count; (count = input.read(chunk)) != -1;) {
                            if (output.size() > MAX_BODY - count) throw new IOException("response exceeds 8 MiB");
                            output.write(chunk, 0, count);
                        }
                    }
                }
                JSONObject result = new JSONObject(), resultHeaders = new JSONObject();
                for (String name : response.headers().names()) resultHeaders.put(name, response.header(name));
                result.put("Success", status >= 200 && status < 300);
                result.put("StatusCode", status);
                result.put("StatusMessage", response.message());
                result.put("Headers", resultHeaders);
                byte[] content = output.toByteArray();
                try {
                    String text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(content)).toString();
                    result.put("Body", text);
                } catch (CharacterCodingException invalidUtf8) {
                    result.put("BodyBase64", Base64.encodeToString(content, Base64.NO_WRAP));
                }
                return result.toString().getBytes(StandardCharsets.UTF_8);
            }
        }
        throw new IOException("HTTP redirect processing failed");
    }

    private static final int MAX_SOCKETS = 16, MAX_MESSAGE = 1024 * 1024, MAX_QUEUED = 4 * MAX_MESSAGE;
    private static final ConcurrentHashMap<String, SocketState> SOCKETS = new ConcurrentHashMap<>();

    private static final class SocketState extends WebSocketListener {
        final CountDownLatch ready = new CountDownLatch(1);
        final ArrayDeque<JSONObject> events = new ArrayDeque<>();
        volatile WebSocket socket;
        volatile boolean closed;
        volatile String failure;
        volatile String redirect;
        volatile long touched = System.nanoTime();
        int queued;

        @Override public void onOpen(WebSocket value, Response response) { socket = value; ready.countDown(); }
        @Override public void onMessage(WebSocket value, String text) {
            enqueue(value, text.getBytes(StandardCharsets.UTF_8), text);
        }
        @Override public void onMessage(WebSocket value, ByteString bytes) { enqueue(value, bytes.toByteArray(), null); }
        private synchronized void enqueue(WebSocket value, byte[] bytes, String text) {
            if (closed) return;
            if (bytes.length > MAX_MESSAGE || queued > MAX_QUEUED - bytes.length || events.size() >= 64) {
                failure = "WebSocket receive limit exceeded"; closed = true; value.cancel(); return;
            }
            try {
                events.add(text == null ? new JSONObject().put("base64", Base64.encodeToString(bytes, Base64.NO_WRAP)) :
                    new JSONObject().put("message", text));
                queued += bytes.length;
            } catch (Exception invalid) { closed = true; value.cancel(); }
        }
        @Override public void onClosing(WebSocket value, int code, String reason) { closed = true; value.close(code, reason); }
        @Override public void onClosed(WebSocket value, int code, String reason) { closed = true; ready.countDown(); }
        @Override public void onFailure(WebSocket value, Throwable problem, Response response) {
            String detail = problem.getClass().getSimpleName() + ": " + String.valueOf(problem.getMessage());
            failure = detail.length() > 192 ? detail.substring(0, 192) : detail;
            if (response != null && (response.code() == 301 || response.code() == 302 ||
                response.code() == 303 || response.code() == 307 || response.code() == 308))
                redirect = response.header("Location");
            closed = true; ready.countDown();
            if (response != null) response.close();
        }
    }

    private static JSONObject socketOperation(JSONObject options) throws Exception {
        String action = options.getString("Action");
        if (action.equals("connect")) {
            String url = options.getString("Url");
            if (!(url.startsWith("ws://") || url.startsWith("wss://"))) throw new IOException("expected a ws(s) URL");
            Request request = new Request.Builder().url(url).build();
            if (!request.url().username().isEmpty() || !request.url().password().isEmpty())
                throw new IOException("URL credentials are not supported");
            String id = UUID.randomUUID().toString();
            SocketState state = new SocketState();
            synchronized (SOCKETS) {
                long now = System.nanoTime();
                for (String key : SOCKETS.keySet()) {
                    SocketState previous = SOCKETS.get(key);
                    if (previous != null && now - previous.touched > TimeUnit.MINUTES.toNanos(2)) {
                        SOCKETS.remove(key, previous);
                        if (previous.socket != null) previous.socket.cancel();
                    }
                }
                if (SOCKETS.size() >= MAX_SOCKETS) throw new IOException("too many WebSocket connections");
                SOCKETS.put(id, state);
            }
            try {
                for (int redirects = 0; redirects <= 10; redirects++) {
                    state.socket = SOCKET_CLIENT.newWebSocket(request, state);
                    if (!state.ready.await(15, TimeUnit.SECONDS)) throw new IOException("WebSocket connect timeout");
                    if (!state.closed && state.failure == null) return new JSONObject().put("id", id);
                    if (state.redirect == null) throw new IOException(state.failure == null ? "WebSocket closed during connect" : state.failure);
                    if (redirects == 10) throw new IOException("too many WebSocket redirects");
                    String location = state.redirect;
                    if (location.startsWith("wss://")) location = "https://" + location.substring(6);
                    else if (location.startsWith("ws://")) location = "http://" + location.substring(5);
                    HttpUrl target = request.url().resolve(location);
                    if (target == null || !target.username().isEmpty() || !target.password().isEmpty() ||
                        (request.url().isHttps() && !target.isHttps()))
                        throw new IOException("unsafe WebSocket redirect");
                    request = request.newBuilder().url(target).build();
                    state.socket.cancel();
                    SocketState next = new SocketState();
                    if (!SOCKETS.replace(id, state, next)) throw new IOException("WebSocket was closed during redirect");
                    state = next;
                }
                throw new IOException("WebSocket redirect processing failed");
            } catch (Exception problem) {
                SOCKETS.remove(id, state);
                if (state.socket != null) state.socket.cancel();
                throw problem;
            }
        }
        String id = options.getString("Id");
        SocketState state = SOCKETS.get(id);
        if (state == null) throw new IOException("WebSocket is closed or unknown");
        state.touched = System.nanoTime();
        synchronized (state) {
            if (action.equals("poll")) {
                JSONArray events = new JSONArray();
                while (!state.events.isEmpty()) events.put(state.events.removeFirst());
                state.queued = 0;
                if (state.closed) SOCKETS.remove(id, state);
                return new JSONObject().put("events", events).put("closed", state.closed);
            }
            if (action.equals("close")) {
                state.closed = true;
                state.socket.close(1000, "");
                return new JSONObject().put("ok", true);
            }
            if (action.equals("send")) {
                String message = options.getString("Message");
                int size = message.getBytes(StandardCharsets.UTF_8).length;
                if (size > MAX_MESSAGE || state.socket.queueSize() > MAX_QUEUED - size)
                    throw new IOException("WebSocket send limit exceeded");
                if (state.closed || !state.socket.send(message)) throw new IOException("WebSocket is closed");
                return new JSONObject().put("ok", true);
            }
        }
        throw new IOException("unsupported WebSocket operation");
    }
}

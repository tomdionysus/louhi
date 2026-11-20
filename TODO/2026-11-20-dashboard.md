# Louhi's status and admin dashboard: the design

the maintainer, 2026-11-20, to Lir, Louhi, Mimir and Kvasir: "You will expose an optional
(configurable) Status/Admin dashboard, served via an API over http, written in
React. The base directory of this react build must be configurable, and default
to your own install. the http server must be SPA capable (routes first, then all
routes serve the SPA), this must also be configurable. You will expose OpenAPI
for the API. Styling and structure should be taken from Lugh's admin interface.
You will allow both a username/password and Lugh for authentication for this."

Where each part lives (the stack's rule, through the Lir Development Cluster
session): the static files and the SPA fallback are Dannan's HTTP core (the maintainer's
Q1); the password check is Dannan's credential helper (its item 11); Lugh
sign-in is lugh_client's relying-party class; the React shell and its styling,
taken from Lugh's admin, live once where the group settles (open). Louhi's own
part is its screens, its API and its OpenAPI document, below.

## Configuration

In `config.yaml`, through Dannan's config reader:
- `dashboard.enabled` (default off) and `dashboard.root`, the built React
  files (default `<prefix>/share/louhi/dashboard`, beside the install);
- `dashboard.spa` (default on): routes first, then every other GET path is
  answered with `index.html`;
- served on the API's listeners, under `/`; the API stays under `/v1/`.

## Signing in

Two ways, either one enough, both off unless configured:
- **Lugh**, as the API already signs in: a bearer token from a trusted issuer,
  audience "louhi", scopes as today (`louhi:admin`, `louhi:<project>:read`...).
  The dashboard uses the authorisation-code flow with PKCE through Lugh, the
  token kept in memory by the page.
- **A username and password**: operator accounts in `secrets.yaml`
  (`dashboard.users: [{name, password_hash, scopes}]`), the hash Dannan's
  credential helper's PHC string, made by `louhi-node --hash-password`. A
  sign-in (`POST /v1/session`) answers a short-lived signed session token
  (HMAC under a key derived from the cluster key, so any node verifies it; no
  session store, no node special), carrying the account's scopes. Sign-in is
  rate-limited per name and address through Dannan's keyed limits.
- Every API route checks the same scopes whichever way the caller signed in.

## The API the dashboard reads

What exists (design, "The endpoints") plus, each behind its scope:
- `GET /v1/status`: this node and the cluster as it sees them (members,
  readiness, the store's state, Lir's connection, the runtime's limits);
- `GET /v1/nodes/{node}`: one node's status, its tasks;
- `GET /v1/projects/{project}/tasks/{task}/events`: a task's recent
  lifecycle, from its statuses;
- `POST /v1/projects/{project}/tasks/{task}/unpark` (`louhi:{project}:run`):
  the operator's action on a parked task (discipline 2), routed to the node
  that runs it;
- `GET /v1/openapi.json`: the OpenAPI 3.1 document for all of `/v1/`, written
  by hand beside the routes and checked by a test against the router's own
  route table, so it cannot drift.

## The screens

- **Cluster**: nodes, their readiness, capacity and tasks; what this node sees.
- **Projects**: quota against use, task definitions, tasks by state.
- **Task**: its definition, every node's status, its events, and stop or
  unpark.
- **Networks** (once step 10 lands): VPCs, subnets, route tables, security
  groups.

## Order

1. The OpenAPI document and its drift test, for the API as it stands (no
   dependency).
2. `/v1/status`, `/v1/nodes/{node}`, task events, unpark (no dependency).
3. Password sign-in and session tokens, on Dannan's credential helper and
   keyed limits (waits on Dannan 0.10.0's items).
4. The screens, on the shared React shell once the group settles where it
   lives.
5. Static serving and the SPA fallback, on Dannan's HTTP core (Q1).

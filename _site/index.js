/* Static download host. The worker only serves the uploaded assets. */
export default {
  async fetch(request, env) {
    return env.ASSETS.fetch(request);
  }
};

// Deliberately ill-typed: `midday script check` must exit 3 with a
// structured file:line diagnostic pointing at the assignment below.
const answer: number = "forty-two";

export { answer };

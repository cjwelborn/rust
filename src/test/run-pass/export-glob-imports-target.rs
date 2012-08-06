// Test that a glob-export functions as an import
// when referenced within its own local scope.

// Modified to not use export since it's going away. --pcw

module foo {
    import bar::*;
    module bar {
        const a : int = 10;
    }
    fn zum() {
        let b = a;
    }
}

fn main() { }

import m::f;
import m::g;

module m {
    export f, g;

    fn f() { }
    fn g() { }
}

fn main() { f(); g(); m::f(); m::g(); }

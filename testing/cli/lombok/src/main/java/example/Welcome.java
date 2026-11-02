package example;

public class Welcome {
    private Greeter greeter;

    public String welcome(Person p) {
        return greeter.greeting(p);
    }
}

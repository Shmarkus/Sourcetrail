package example.web;

import example.Caller;
import example.Person;

@Endpoint
public class GreetingController {
    private final Caller caller = new Caller();

    public String greet(Person person) {
        return caller.greeting(person);
    }
}

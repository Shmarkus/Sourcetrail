package example;

import lombok.RequiredArgsConstructor;

@RequiredArgsConstructor
public class CallerIntegrationTest {
    private final Caller caller;

    public String greetsPerson() {
        return caller.greeting(new Person());
    }
}

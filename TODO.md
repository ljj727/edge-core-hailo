# TODO List

## Phase 1: 프로젝트 기본 구조 (완료 ✅)
- [x] 프로젝트 디렉토리 생성
- [x] README.md 작성
- [x] ARCHITECTURE.md 작성
- [x] BUILD.md 작성
- [x] API.md 작성
- [x] GETTING_STARTED.md 작성
- [x] proto/stream.proto 정의
- [x] CMakeLists.txt 기본 설정
- [x] 의존성 설치 스크립트
- [x] .gitignore 설정

## Phase 2: StreamProcessor 구현 (다음 단계)
- [ ] include/stream_processor.h 작성
- [ ] src/stream_processor.cpp 구현
  - [ ] GStreamer 파이프라인 생성
  - [ ] Hailo plugin 연동
  - [ ] Probe callback 구현
  - [ ] 에러 핸들링
- [ ] 단일 스트림 테스트
- [ ] 자동 재연결 로직

## Phase 3: StreamManager 구현
- [ ] include/stream_manager.h 작성
- [ ] src/stream_manager.cpp 구현
  - [ ] 멀티스트림 관리
  - [ ] GLib MainLoop 통합
  - [ ] 스레드 안전성 보장
- [ ] 동시 4개 스트림 테스트

## Phase 4: NATS 통합
- [ ] include/nats_publisher.h 작성
- [ ] src/nats_publisher.cpp 구현
  - [ ] NATS 연결 관리
  - [ ] JSON 직렬화
  - [ ] 메시지 발행
- [ ] NATS 메시지 포맷 검증

## Phase 5: gRPC 서버
- [ ] include/grpc_server.h 작성
- [ ] src/grpc_server.cpp 구현
  - [ ] StreamServiceImpl 구현
  - [ ] 모든 RPC 메서드 구현
- [ ] grpcurl로 API 테스트

## Phase 6: Main 함수 및 통합
- [ ] src/main.cpp 작성
  - [ ] 명령행 인자 파싱
  - [ ] 설정 파일 로딩
  - [ ] 컴포넌트 초기화
  - [ ] 시그널 핸들링 (SIGINT, SIGTERM)
- [ ] 전체 통합 테스트

## Phase 7: 최적화 및 안정화
- [ ] 메모리 leak 체크 (Valgrind)
- [ ] 성능 프로파일링 (perf)
- [ ] 에러 복구 테스트
- [ ] 장시간 안정성 테스트 (24시간+)

## Phase 8: 문서화
- [ ] API 사용 예시 추가
- [ ] 트러블슈팅 가이드
- [ ] 성능 튜닝 가이드
- [ ] 배포 가이드

## Phase 9: Docker 및 배포
- [ ] Dockerfile 작성
- [ ] docker-compose.yml 작성
- [ ] CI/CD 파이프라인 (GitHub Actions)
- [ ] 릴리스 스크립트

## Phase 10: 테스트
- [ ] 단위 테스트 (Google Test)
- [ ] 통합 테스트
- [ ] 부하 테스트
- [ ] 장애 복구 테스트

## 선택적 기능 (Nice to Have)
- [ ] TLS/SSL 지원 (gRPC, NATS)
- [ ] 인증/인가 (API 키)
- [ ] 메트릭 수집 (Prometheus)
- [ ] 웹 대시보드
- [ ] 로그 집계 (ELK Stack)
- [ ] 설정 파일 (YAML/JSON)
- [ ] Health check endpoint
- [ ] Graceful shutdown
- [ ] 다중 Hailo 디바이스 지원

## 버그 추적
- [ ] (버그 발견 시 여기에 추가)

## 개선 아이디어
- [ ] (개선 아이디어 여기에 추가)

---

**현재 진행 상황:** Phase 1 완료 ✅  
**다음 작업:** Phase 2 - StreamProcessor 구현 시작
